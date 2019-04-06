#include "key_generator.h"

#include <random>

#include "benchmark.h"

class NormalGenerator final : public KeyGenerator {
  public:
    NormalGenerator(lru_key_t max_key)
        : max_key(max_key), gen(42), dist(max_key / 2., 0.315 * max_key / 2) {}

    std::string name() const override { return "normal"; }

    ptr_t clone() const override { return KeyGenerator::ptr_t(new NormalGenerator(*this)); }

    void setThread(size_t seed) override { gen.seed(seed); }

    lru_key_t getKey() override {
        return std::min(static_cast<lru_key_t>(std::abs(dist(gen))), max_key);
    }

    lru_key_t                        max_key;
    std::mt19937                     gen;
    std::normal_distribution<double> dist;
};

class UniformGenerator final : public KeyGenerator {
  public:
    UniformGenerator(lru_key_t max_key) : gen(42), dist(0, max_key - 1) {}

    std::string name() const override { return "uniform"; }

    ptr_t clone() const override { return KeyGenerator::ptr_t(new UniformGenerator(*this)); }

    void setThread(size_t seed) override { gen.seed(seed); }

    lru_key_t getKey() override { return dist(gen); }

    std::mt19937                             gen;
    std::uniform_int_distribution<lru_key_t> dist;
};

class ExpGenerator final : public KeyGenerator {
  public:
    ExpGenerator(size_t capacity, float alpha) : gen(42), dist(getLambda(capacity, alpha)) {}

    std::string name() const override { return "exp"; }

    ptr_t clone() const override { return KeyGenerator::ptr_t(new ExpGenerator(*this)); }

    void setThread(size_t seed) override { gen.seed(seed); }

    lru_key_t getKey() override { return static_cast<lru_key_t>(dist(gen)); }

    static double getLambda(size_t interval, double area_under_interval) {
        return -std::log(1 - area_under_interval) / interval;
    }

    std::mt19937 gen;
    std::exponential_distribution<double> dist;
};

class SameGenerator final : public KeyGenerator {
  public:
    SameGenerator() : state_(0) {}

    SameGenerator(size_t state) : state_(state) {}

    std::string name() const override { return "same"; }

    ptr_t clone() const override { return KeyGenerator::ptr_t(new SameGenerator(*this)); }

    void setThread(size_t thread) override {}

    lru_key_t getKey() override { return state_++; }

    size_t state_;
};

class DisjointGenerator final : public KeyGenerator {
  public:
    DisjointGenerator() : state_(0) {}

    DisjointGenerator(size_t state) : state_(state) {}

    std::string name() const override { return "disjoint"; }

    ptr_t clone() const override { return KeyGenerator::ptr_t(new DisjointGenerator(*this)); }

    void setThread(size_t thread) override { state_ = thread * (1 << 30); }

    lru_key_t getKey() override { return state_++; }

    size_t state_;
};

KeyGenerator::ptr_t KeyGenerator::factory(BenchmarkApp& b, const std::string& name,
                                          lru_key_t max_key) {
    if (name == "normal") {
        return ptr_t(new NormalGenerator(max_key));
    }
    if (name == "uniform") {
        return ptr_t(new UniformGenerator(max_key));
    }
    if (name == "same") {
        return ptr_t(new SameGenerator());
    }
    if (name == "disjoint") {
        return ptr_t(new DisjointGenerator());
    }
    if (name == "exp") {
        return ptr_t(new ExpGenerator(b.capacity, 0.8));
    }
    throw std::runtime_error("Unknown generator: " + name);
}
