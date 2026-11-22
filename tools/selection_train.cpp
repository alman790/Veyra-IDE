#include "ggml.h"
#include "gguf.h"
#include "ts/LocalCodeModel.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

struct Sample {
    ts::SelectionFeatures features;
    int target, classes;
    QString split;
};

struct Head {
    int dimensions, rank;
    std::vector<float> a, b;
    std::vector<double> ma, va, mb, vb;
    int step = 0;

    Head(int d, int r)
        : dimensions(d), rank(r), a(d * r), b(9 * r), ma(d * r), va(d * r), mb(9 * r), vb(9 * r) {
        std::mt19937 random(790);
        std::normal_distribution<float> initial(0, 0.02f);
        for (auto& weight : a)
            weight = initial(random);
    }

    std::vector<double> hidden(const Sample& sample) const {
        std::vector<double> h(rank);
        for (int r = 0; r < rank; ++r)
            for (int d = 0; d < dimensions; ++d)
                h[r] += a[r * dimensions + d] * sample.features.embedding[d] / std::sqrt(double(dimensions));
        return h;
    }

    std::vector<double> probabilities(const Sample& sample) const {
        const auto h = hidden(sample);
        std::vector<double> output(sample.classes);
        for (int c = 0; c < sample.classes; ++c) {
            output[c] = sample.features.logits[c];
            for (int r = 0; r < rank; ++r)
                output[c] += b[c * rank + r] * h[r];
        }
        const auto largest = *std::max_element(output.begin(), output.end());
        double total = 0;
        for (auto& p : output)
            total += (p = std::exp(p - largest));
        for (auto& p : output)
            p /= total;
        return output;
    }

    void train(const std::vector<Sample>& samples) {
        std::vector<double> ga(a.size()), gb(b.size());
        int count = 0;
        for (const auto& sample : samples)
            if (sample.split == "train") {
                ++count;
                const auto h = hidden(sample), p = probabilities(sample);
                std::vector<double> dh(rank);
                for (int c = 0; c < sample.classes; ++c) {
                    const double delta = p[c] - (sample.target == c ? 1 : 0);
                    for (int r = 0; r < rank; ++r) {
                        gb[c * rank + r] += delta * h[r];
                        dh[r] += delta * b[c * rank + r];
                    }
                }
                for (int r = 0; r < rank; ++r)
                    for (int d = 0; d < dimensions; ++d)
                        ga[r * dimensions + d] +=
                            dh[r] * sample.features.embedding[d] / std::sqrt(double(dimensions));
            }
        ++step;
        auto update = [&](auto& weights, auto& first, auto& second, auto& gradients) {
            for (std::size_t i = 0; i < weights.size(); ++i) {
                const double g = gradients[i] / count + 0.0005 * weights[i];
                first[i] = 0.9 * first[i] + 0.1 * g;
                second[i] = 0.999 * second[i] + 0.001 * g * g;
                weights[i] -= 0.005 * (first[i] / (1 - std::pow(0.9, step))) /
                              (std::sqrt(second[i] / (1 - std::pow(0.999, step))) + 1e-8);
            }
        };
        update(a, ma, va, ga);
        update(b, mb, vb, gb);
    }

    QJsonObject evaluate(const std::vector<Sample>& samples, const QString& split) const {
        int count = 0, correct = 0, baseline = 0;
        double loss = 0;
        for (const auto& sample : samples)
            if (sample.split == split) {
                ++count;
                const auto p = probabilities(sample);
                correct += std::max_element(p.begin(), p.end()) - p.begin() == sample.target;
                baseline += std::max_element(sample.features.logits.begin(),
                                             sample.features.logits.begin() + sample.classes) -
                                sample.features.logits.begin() ==
                            sample.target;
                loss -= std::log(std::max(1e-12, p[sample.target]));
            }
        if (!count)
            throw std::runtime_error("Each split must contain examples");
        return {{"examples", count},
                {"correct", correct},
                {"baseline_correct", baseline},
                {"cross_entropy", loss / count}};
    }

    void save(const QString& path, const Sample& sample) const {
        auto* tensors = ggml_init({2 * ggml_tensor_overhead() + 1024, nullptr, true});
        if (!tensors)
            throw std::runtime_error("Allocate adapter metadata");
        auto* metadata = gguf_init_empty();
        gguf_set_val_str(metadata, "general.type", "adapter");
        gguf_set_val_str(metadata, "general.architecture", sample.features.architecture.c_str());
        gguf_set_val_str(metadata, "general.name", "Veyra concurrency selection head");
        gguf_set_val_str(metadata, "adapter.type", "lora");
        gguf_set_val_f32(metadata, "adapter.lora.alpha", float(rank));
        auto savedA = a;
        for (auto& value : savedA)
            value /= std::sqrt(float(dimensions));
        std::vector<float> savedB(sample.features.vocabularySize * rank);
        for (int c = 0; c < 9; ++c)
            for (int r = 0; r < rank; ++r)
                savedB[sample.features.tokenIds[c] * rank + r] = b[c * rank + r];
        auto* tensorA = ggml_new_tensor_2d(tensors, GGML_TYPE_F32, dimensions, rank);
        auto* tensorB = ggml_new_tensor_2d(tensors, GGML_TYPE_F32, rank, sample.features.vocabularySize);
        tensorA->data = savedA.data();
        tensorB->data = savedB.data();
        ggml_set_name(tensorA, "output.weight.lora_a");
        ggml_set_name(tensorB, "output.weight.lora_b");
        gguf_add_tensor(metadata, tensorA);
        gguf_add_tensor(metadata, tensorB);
        const bool ok = gguf_write_to_file(metadata, path.toUtf8().constData(), false);
        gguf_free(metadata);
        ggml_free(tensors);
        if (!ok)
            throw std::runtime_error("Write trained adapter");
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3) {
        std::cerr << "Usage: veyra_selection_train dataset.json output-prefix\n";
        return 2;
    }
    try {
        QFile data(QString::fromLocal8Bit(argv[1]));
        if (!data.open(QIODevice::ReadOnly))
            throw std::runtime_error("Open training fixture");
        const auto bytes = data.readAll();
        const auto document = QJsonDocument::fromJson(bytes);
        if (!document.isArray())
            throw std::runtime_error("The dataset must be a JSON array");
        const auto items = document.array();
        QMap<QString, QString> families, sources;
        std::vector<Sample> samples;
        for (const auto& raw : items) {
            const auto item = raw.toObject();
            const auto split = item["split"].toString(), family = item["family"].toString(),
                       source = item["source"].toString();
            if ((split != "train" && split != "validation" && split != "test") || family.isEmpty() ||
                source.isEmpty())
                throw std::runtime_error("Each sample needs a split, family and source");
            if ((families.contains(family) && families[family] != split) ||
                (sources.contains(source) && sources[source] != split))
                throw std::runtime_error(
                    "Source families and identical source must not cross dataset splits");
            families[family] = sources[source] = split;
            std::vector<std::string> candidates;
            for (const auto& c : item["candidates"].toArray())
                candidates.push_back(c.toString().toStdString());
            const int target = item["target"].toInt(-1);
            if (candidates.empty() || candidates.size() > 8 || target < 0 || target > int(candidates.size()))
                throw std::runtime_error("Invalid training label");
            auto features = ts::LocalCodeModel::selectionFeatures(item["source"].toString().toStdString(),
                                                                  candidates, [] { return false; });
            if (features.embedding.empty() || features.logits.size() != 9)
                throw std::runtime_error("Extract frozen model features");
            samples.push_back(
                {std::move(features), target, int(candidates.size()) + 1, item["split"].toString()});
            if (samples.size() % 20 == 0)
                std::cerr << "Features " << samples.size() << '/' << items.size() << '\n';
        }
        if (samples.empty())
            throw std::runtime_error("Empty training fixture");
        Head head(samples[0].features.embedding.size(), 8), best = head;
        head.evaluate(samples, "train");
        head.evaluate(samples, "validation");
        head.evaluate(samples, "test");
        double bestLoss = 1e100;
        int bestEpoch = 0;
        for (int epoch = 0; epoch < 500; ++epoch) {
            head.train(samples);
            if (epoch % 10 == 0) {
                const auto validation = head.evaluate(samples, "validation");
                const double loss = validation["cross_entropy"].toDouble();
                if (loss < bestLoss) {
                    bestLoss = loss;
                    best = head;
                    bestEpoch = epoch + 1;
                }
                if (epoch - bestEpoch > 80)
                    break;
            }
        }
        const QString prefix = QString::fromLocal8Bit(argv[2]);
        const auto validation = best.evaluate(samples, "validation"), test = best.evaluate(samples, "test");
        const bool accepted = validation["correct"].toInt() > validation["baseline_correct"].toInt() &&
                              test["correct"].toInt() > test["baseline_correct"].toInt();
        if (accepted)
            best.save(prefix + ".gguf", samples[0]);
        const QJsonObject report{
            {"accepted", accepted},
            {"dataset_sha256",
             QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {"scope", "output projection LoRA; transformer weights frozen"},
            {"seed", 790},
            {"learning_rate", 0.005},
            {"rank", head.rank},
            {"dimensions", head.dimensions},
            {"best_epoch", bestEpoch},
            {"train", best.evaluate(samples, "train")},
            {"validation", best.evaluate(samples, "validation")},
            {"test", best.evaluate(samples, "test")}};
        QFile result(prefix + ".json");
        if (!result.open(QIODevice::WriteOnly))
            throw std::runtime_error("Write training report");
        result.write(QJsonDocument(report).toJson());
        std::cout << QJsonDocument(report).toJson().constData();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
