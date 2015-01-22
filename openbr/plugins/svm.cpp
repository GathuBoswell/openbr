/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2012 The MITRE Corporation                                      *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <QTemporaryFile>
#include <opencv2/core/core.hpp>
#include <opencv2/ml/ml.hpp>

#include "openbr_internal.h"
#include "openbr/core/opencvutils.h"

using namespace cv;

namespace br
{

static void storeSVM(const SVM &svm, QDataStream &stream)
{
    // Create local file
    QTemporaryFile tempFile;
    tempFile.open();
    tempFile.close();

    // Save SVM to local file
    svm.save(qPrintable(tempFile.fileName()));

    // Copy local file contents to stream
    tempFile.open();
    QByteArray data = tempFile.readAll();
    tempFile.close();
    stream << data;
}

static void loadSVM(SVM &svm, QDataStream &stream)
{
    // Copy local file contents from stream
    QByteArray data;
    stream >> data;

    // Create local file
    QTemporaryFile tempFile(QDir::tempPath()+"/SVM");
    tempFile.open();
    tempFile.write(data);
    tempFile.close();

    // Load SVM from local file
    svm.load(qPrintable(tempFile.fileName()));
}

static void trainSVM(SVM &svm, Mat data, Mat lab, int kernel, int type, float C, float gamma, int folds, bool balanceFolds, int termCriteria)
{
    if (data.type() != CV_32FC1)
        qFatal("Expected single channel floating point training data.");

    CvSVMParams params;
    params.kernel_type = kernel;
    params.svm_type = type;
    params.p = 0.1;
    params.nu = 0.5;
    params.term_crit = cvTermCriteria(CV_TERMCRIT_ITER+CV_TERMCRIT_EPS, termCriteria, FLT_EPSILON);

    if ((C == -1) || ((gamma == -1) && (kernel == CvSVM::RBF))) {
        try {
            svm.train_auto(data, lab, Mat(), Mat(), params, folds,
                           CvSVM::get_default_grid(CvSVM::C),
                           CvSVM::get_default_grid(CvSVM::GAMMA),
                           CvSVM::get_default_grid(CvSVM::P),
                           CvSVM::get_default_grid(CvSVM::NU),
                           CvSVM::get_default_grid(CvSVM::COEF),
                           CvSVM::get_default_grid(CvSVM::DEGREE),
                           balanceFolds);
        } catch (...) {
            qWarning("Some classes do not contain sufficient examples or are not discriminative enough for accurate SVM classification.");
            svm.train(data, lab, Mat(), Mat(), params);
        }
    } else {
        params.C = C;
        params.gamma = gamma;
        svm.train(data, lab, Mat(), Mat(), params);
    }

    CvSVMParams p = svm.get_params();
    qDebug("SVM C = %f  Gamma = %f  Support Vectors = %d", p.C, p.gamma, svm.get_support_vector_count());
}

/*!
 * \ingroup transforms
 * \brief C. Burges. "A tutorial on support vector machines for pattern recognition,"
 * \author Josh Klontz \cite jklontz
 * Knowledge Discovery and Data Mining 2(2), 1998.
 */
class SVMTransform : public Transform
{
    Q_OBJECT
    Q_ENUMS(Kernel)
    Q_ENUMS(Type)
    Q_PROPERTY(Kernel kernel READ get_kernel WRITE set_kernel RESET reset_kernel STORED false)
    Q_PROPERTY(Type type READ get_type WRITE set_type RESET reset_type STORED false)
    Q_PROPERTY(float C READ get_C WRITE set_C RESET reset_C STORED false)
    Q_PROPERTY(float gamma READ get_gamma WRITE set_gamma RESET reset_gamma STORED false)
    Q_PROPERTY(QString inputVariable READ get_inputVariable WRITE set_inputVariable RESET reset_inputVariable STORED false)
    Q_PROPERTY(QString outputVariable READ get_outputVariable WRITE set_outputVariable RESET reset_outputVariable STORED false)
    Q_PROPERTY(bool returnDFVal READ get_returnDFVal WRITE set_returnDFVal RESET reset_returnDFVal STORED false)
    Q_PROPERTY(int termCriteria READ get_termCriteria WRITE set_termCriteria RESET reset_termCriteria STORED false)
    Q_PROPERTY(int folds READ get_folds WRITE set_folds RESET reset_folds STORED false)
    Q_PROPERTY(bool balanceFolds READ get_balanceFolds WRITE set_balanceFolds RESET reset_balanceFolds STORED false)

public:
    enum Kernel { Linear = CvSVM::LINEAR,
                  Poly = CvSVM::POLY,
                  RBF = CvSVM::RBF,
                  Sigmoid = CvSVM::SIGMOID };

    enum Type { C_SVC = CvSVM::C_SVC,
                NU_SVC = CvSVM::NU_SVC,
                ONE_CLASS = CvSVM::ONE_CLASS,
                EPS_SVR = CvSVM::EPS_SVR,
                NU_SVR = CvSVM::NU_SVR};

private:
    BR_PROPERTY(Kernel, kernel, Linear)
    BR_PROPERTY(Type, type, C_SVC)
    BR_PROPERTY(float, C, -1)
    BR_PROPERTY(float, gamma, -1)
    BR_PROPERTY(QString, inputVariable, "Label")
    BR_PROPERTY(QString, outputVariable, "")
    BR_PROPERTY(bool, returnDFVal, false)
    BR_PROPERTY(int, termCriteria, 1000)
    BR_PROPERTY(int, folds, 5)
    BR_PROPERTY(bool, balanceFolds, false)

    SVM svm;
    QHash<QString, int> labelMap;
    QHash<int, QVariant> reverseLookup;

    void train(const TemplateList &_data)
    {
        Mat data = OpenCVUtils::toMat(_data.data());
        Mat lab;
        // If we are doing regression, the input variable should have float
        // values
        if (type == EPS_SVR || type == NU_SVR) {
            lab = OpenCVUtils::toMat(File::get<float>(_data, inputVariable));
        }
        // If we are doing classification, we should be dealing with discrete
        // values. Map them and store the mapping data
        else {
            QList<int> dataLabels = _data.indexProperty(inputVariable, labelMap, reverseLookup);
            lab = OpenCVUtils::toMat(dataLabels);
        }

        trainSVM(svm, data, lab, kernel, type, C, gamma, folds, balanceFolds, termCriteria);
    }

    void project(const Template &src, Template &dst) const
    {
        if (returnDFVal && reverseLookup.size() > 2)
            qFatal("Decision function for multiclass classification not implemented.");

        dst = src;
        float prediction = svm.predict(src.m().reshape(1, 1), returnDFVal);
        if (returnDFVal) {
            dst.m() = Mat(1, 1, CV_32F);
            dst.m().at<float>(0, 0) = prediction;
            // positive values ==> first class
            // negative values ==> second class
            if (type != EPS_SVR && type != NU_SVR)
                prediction = prediction > 0 ? 0 : 1;
        }
        if (type == EPS_SVR || type == NU_SVR) {
            dst.file.set(outputVariable, prediction);
            dst.m() = Mat(1, 1, CV_32F);
            dst.m().at<float>(0, 0) = prediction;

        } else
            dst.file.set(outputVariable, reverseLookup[prediction]);
    }

    void store(QDataStream &stream) const
    {
        storeSVM(svm, stream);
        stream << labelMap << reverseLookup;
    }

    void load(QDataStream &stream)
    {
        loadSVM(svm, stream);
        stream >> labelMap >> reverseLookup;
    }

    void init()
    {
        if (outputVariable.isEmpty())
            outputVariable = inputVariable;
    }
};

BR_REGISTER(Transform, SVMTransform)

class SVMClassifier : public Classifier
{
    Q_OBJECT
    Q_ENUMS(Kernel)
    Q_ENUMS(Type)

    Q_PROPERTY(br::Representation* rep READ get_rep WRITE set_rep RESET reset_rep STORED false)
    Q_PROPERTY(Kernel kernel READ get_kernel WRITE set_kernel RESET reset_kernel STORED false)
    Q_PROPERTY(Type type READ get_type WRITE set_type RESET reset_type STORED false)
    Q_PROPERTY(float C READ get_C WRITE set_C RESET reset_C STORED false)
    Q_PROPERTY(float gamma READ get_gamma WRITE set_gamma RESET reset_gamma STORED false)
    Q_PROPERTY(QString inputVariable READ get_inputVariable WRITE set_inputVariable RESET reset_inputVariable STORED false)
    Q_PROPERTY(QString outputVariable READ get_outputVariable WRITE set_outputVariable RESET reset_outputVariable STORED false)
    Q_PROPERTY(bool returnDFVal READ get_returnDFVal WRITE set_returnDFVal RESET reset_returnDFVal STORED false)
    Q_PROPERTY(int termCriteria READ get_termCriteria WRITE set_termCriteria RESET reset_termCriteria STORED false)
    Q_PROPERTY(int folds READ get_folds WRITE set_folds RESET reset_folds STORED false)
    Q_PROPERTY(bool balanceFolds READ get_balanceFolds WRITE set_balanceFolds RESET reset_balanceFolds STORED false)

public:
    enum Kernel { Linear = CvSVM::LINEAR,
                  Poly = CvSVM::POLY,
                  RBF = CvSVM::RBF,
                  Sigmoid = CvSVM::SIGMOID };

    enum Type { C_SVC = CvSVM::C_SVC,
                NU_SVC = CvSVM::NU_SVC,
                ONE_CLASS = CvSVM::ONE_CLASS,
                EPS_SVR = CvSVM::EPS_SVR,
                NU_SVR = CvSVM::NU_SVR};

private:
    BR_PROPERTY(br::Representation*, rep, NULL)
    BR_PROPERTY(Kernel, kernel, Linear)
    BR_PROPERTY(Type, type, C_SVC)
    BR_PROPERTY(float, C, -1)
    BR_PROPERTY(float, gamma, -1)
    BR_PROPERTY(QString, inputVariable, "Label")
    BR_PROPERTY(QString, outputVariable, "")
    BR_PROPERTY(bool, returnDFVal, false)
    BR_PROPERTY(int, termCriteria, 1000)
    BR_PROPERTY(int, folds, 5)
    BR_PROPERTY(bool, balanceFolds, false)

    SVM svm;
    QHash<QString, int> labelMap;
    QHash<int, QVariant> reverseLookup;
    void train(const QList<Mat> &images, const QList<float> &labels)
    {
        rep->train(images,labels);
        QList<Mat> representedImages;
        foreach(const Mat &image, images)
            representedImages.append(rep->evaluate(image));

        Mat samples = OpenCVUtils::toMat(representedImages);
        Mat lab = OpenCVUtils::toMat(labels);

        CvSVMParams params;
        params.kernel_type = kernel;
        params.svm_type = type;
        params.p = 0.1;
        params.nu = 0.5;
        params.term_crit = cvTermCriteria(CV_TERMCRIT_ITER+CV_TERMCRIT_EPS, termCriteria, FLT_EPSILON);

        if ((C == -1) || ((gamma == -1) && (kernel == CvSVM::RBF))) {
            try {
                svm.train_auto(samples, lab, Mat(), Mat(), params, folds,
                               CvSVM::get_default_grid(CvSVM::C),
                               CvSVM::get_default_grid(CvSVM::GAMMA),
                               CvSVM::get_default_grid(CvSVM::P),
                               CvSVM::get_default_grid(CvSVM::NU),
                               CvSVM::get_default_grid(CvSVM::COEF),
                               CvSVM::get_default_grid(CvSVM::DEGREE),
                               balanceFolds);
            } catch (...) {
                qWarning("Some classes do not contain sufficient examples or are not discriminative enough for accurate SVM classification.");
                svm.train(samples, lab, Mat(), Mat(), params);
            }
        } else {
            params.C = C;
            params.gamma = gamma;
            svm.train(samples, lab, Mat(), Mat(), params);
        }

        CvSVMParams p = svm.get_params();
        qDebug("SVM C = %f  Gamma = %f  Support Vectors = %d", p.C, p.gamma, svm.get_support_vector_count());
    }

    float classify(const Mat &image) const
    {
        float prediction = -svm.predict(rep->evaluate(image), returnDFVal);
        return prediction;
    }

    void store(QDataStream &stream) const
    {
        storeSVM(svm, stream);
        stream << labelMap << reverseLookup;
    }

    void load(QDataStream &stream)
    {
        loadSVM(svm, stream);
        stream >> labelMap >> reverseLookup;
    }
};

BR_REGISTER(Classifier, SVMClassifier)

/*!
 * \ingroup Distances
 * \brief SVM Regression on template absolute differences.
 * \author Josh Klontz
 */
class SVMDistance : public Distance
{
    Q_OBJECT
    Q_ENUMS(Kernel)
    Q_ENUMS(Type)
    Q_PROPERTY(Kernel kernel READ get_kernel WRITE set_kernel RESET reset_kernel STORED false)
    Q_PROPERTY(Type type READ get_type WRITE set_type RESET reset_type STORED false)
    Q_PROPERTY(QString inputVariable READ get_inputVariable WRITE set_inputVariable RESET reset_inputVariable STORED false)
    Q_PROPERTY(int termCriteria READ get_termCriteria WRITE set_termCriteria RESET reset_termCriteria STORED false)
    Q_PROPERTY(int folds READ get_folds WRITE set_folds RESET reset_folds STORED false)
    Q_PROPERTY(bool balanceFolds READ get_balanceFolds WRITE set_balanceFolds RESET reset_balanceFolds STORED false)

public:
    enum Kernel { Linear = CvSVM::LINEAR,
                  Poly = CvSVM::POLY,
                  RBF = CvSVM::RBF,
                  Sigmoid = CvSVM::SIGMOID };

    enum Type { C_SVC = CvSVM::C_SVC,
                NU_SVC = CvSVM::NU_SVC,
                ONE_CLASS = CvSVM::ONE_CLASS,
                EPS_SVR = CvSVM::EPS_SVR,
                NU_SVR = CvSVM::NU_SVR};

private:
    BR_PROPERTY(Kernel, kernel, Linear)
    BR_PROPERTY(Type, type, EPS_SVR)
    BR_PROPERTY(QString, inputVariable, "Label")
    BR_PROPERTY(int, termCriteria, 1000)
    BR_PROPERTY(int, folds, 5)
    BR_PROPERTY(bool, balanceFolds, false)

    SVM svm;

    void train(const TemplateList &src)
    {
        const Mat data = OpenCVUtils::toMat(src.data());
        const QList<int> lab = src.indexProperty(inputVariable);

        const int instances = data.rows * (data.rows+1) / 2;
        Mat deltaData(instances, data.cols, data.type());
        Mat deltaLab(instances, 1, CV_32FC1);
        int index = 0;
        for (int i=0; i<data.rows; i++)
            for (int j=i; j<data.rows; j++) {
                const bool match = lab[i] == lab[j];
                if (!match && (type == ONE_CLASS))
                    continue;
                absdiff(data.row(i), data.row(j), deltaData.row(index));
                deltaLab.at<float>(index, 0) = (match ? 1 : 0);
                index++;
            }
        deltaData = deltaData.rowRange(0, index);
        deltaLab = deltaLab.rowRange(0, index);

        trainSVM(svm, deltaData, deltaLab, kernel, type, -1, -1, folds, balanceFolds, termCriteria);
    }

    float compare(const Mat &a, const Mat &b) const
    {
        Mat delta;
        absdiff(a, b, delta);
        return svm.predict(delta.reshape(1, 1));
    }

    void store(QDataStream &stream) const
    {
        storeSVM(svm, stream);
    }

    void load(QDataStream &stream)
    {
        loadSVM(svm, stream);
    }
};

BR_REGISTER(Distance, SVMDistance)

} // namespace br

#include "svm.moc"
