// The C++ wrapper over anvil.benchmark v1 (anvil_benchmark.h,
// docs/spec/benchmark-v1.md §5). Header-only, C++20, on top of anvil.hpp.
//
//   void load(anvil::Host& host) override {
//       bench_ = anvil::Benchmark(host);  // inside load(): declaring joins
//       static constexpr const char* kModes[] = {"off", "on", "verify"};
//       bench_.declare({
//           anvil::Benchmark::enum_field("warmup.mode", kModes, "Warm-up mode"),
//           anvil::Benchmark::int_field("warmup.opened", ANVIL_BENCHMARK_UNIT_COUNT, "Files opened"),
//           anvil::Benchmark::number_field("warmup.time", ANVIL_BENCHMARK_UNIT_MS),
//       });
//   }
//   ... later, on any thread: bench_.set("warmup.opened", n); bench_.done();
//
// Nothing here throws, a declaration included: a refused call returns its
// error and Anvil logs one warning, so a plugin never fails to load over its
// benchmark fields. On an Anvil without the service (older than 1.4) every
// call does nothing and returns ANVIL_ERROR_UNSUPPORTED; active() is false.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <anvil/anvil.hpp>
#include <anvil/anvil_benchmark.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace anvil {

class Benchmark {
public:
    // No service: every call does nothing.
    Benchmark() = default;
    // Looks the service up; works once the plugin is loading.
    explicit Benchmark(const Host& host) noexcept
        : service_(lookup(host)), self_(host.self()) {}
    // The service and the plugin's handle as given (for tests and C-style code).
    Benchmark(const AnvilBenchmarkService* service, AnvilPlugin* self) noexcept : service_(service), self_(self) {}

    // Anvil provides anvil.benchmark (1.4 and later).
    bool available() const noexcept { return service_ != nullptr; }
    // This is a benchmark start.
    bool active() const noexcept { return service_ != nullptr && service_->active() != 0; }

    // --- fields to declare inside load() -------------------------------------
    static constexpr AnvilBenchmarkField int_field(const char* name, AnvilBenchmarkUnit unit = ANVIL_BENCHMARK_UNIT_NONE,
                                                   const char* label = nullptr) noexcept {
        return {name, ANVIL_BENCHMARK_INT, static_cast<std::uint32_t>(unit), label, nullptr, 0};
    }
    static constexpr AnvilBenchmarkField number_field(const char* name,
                                                      AnvilBenchmarkUnit unit = ANVIL_BENCHMARK_UNIT_NONE,
                                                      const char* label = nullptr) noexcept {
        return {name, ANVIL_BENCHMARK_NUMBER, static_cast<std::uint32_t>(unit), label, nullptr, 0};
    }
    static constexpr AnvilBenchmarkField flag_field(const char* name, const char* label = nullptr) noexcept {
        return {name, ANVIL_BENCHMARK_FLAG, ANVIL_BENCHMARK_UNIT_NONE, label, nullptr, 0};
    }
    // `options` only has to live until declare() returns.
    template <std::size_t N>
    static constexpr AnvilBenchmarkField enum_field(const char* name, const char* const (&options)[N],
                                                    const char* label = nullptr) noexcept {
        return {name, ANVIL_BENCHMARK_ENUM, ANVIL_BENCHMARK_UNIT_NONE, label, options, static_cast<std::uint32_t>(N)};
    }

    // Inside load() only; makes the plugin a participant, which must call
    // done() once. Zero fields join too.
    AnvilResult declare(std::span<const AnvilBenchmarkField> fields) const noexcept {
        if (service_ == nullptr) return ANVIL_ERROR_UNSUPPORTED;
        return service_->declare(self_, fields.data(), static_cast<std::uint32_t>(fields.size()));
    }
    AnvilResult declare(std::initializer_list<AnvilBenchmarkField> fields) const noexcept {
        return declare(std::span<const AnvilBenchmarkField>(fields.begin(), fields.size()));
    }

    // --- values, from any thread; the last one wins ---------------------------
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    AnvilResult set(const char* field, T value) const noexcept {
        if (service_ == nullptr) return ANVIL_ERROR_UNSUPPORTED;
        return service_->set_int(self_, field, static_cast<std::int64_t>(value));
    }
    template <std::floating_point T> AnvilResult set(const char* field, T value) const noexcept {
        if (service_ == nullptr) return ANVIL_ERROR_UNSUPPORTED;
        return service_->set_number(self_, field, static_cast<double>(value));
    }
    AnvilResult set(const char* field, bool value) const noexcept {
        if (service_ == nullptr) return ANVIL_ERROR_UNSUPPORTED;
        return service_->set_flag(self_, field, value ? 1 : 0);
    }
    AnvilResult set_enum(const char* field, const char* option) const noexcept {
        if (service_ == nullptr) return ANVIL_ERROR_UNSUPPORTED;
        return service_->set_enum(self_, field, option);
    }

    // --- the start ---------------------------------------------------------------
    AnvilResult done() const noexcept { return service_ != nullptr ? service_->done(self_) : ANVIL_ERROR_UNSUPPORTED; }
    AnvilResult learning(const char* reason) const noexcept {
        return service_ != nullptr ? service_->learning(self_, reason) : ANVIL_ERROR_UNSUPPORTED;
    }
    AnvilResult open_cost(std::uint64_t files, double ms) const noexcept {
        return service_ != nullptr ? service_->open_cost(self_, files, ms) : ANVIL_ERROR_UNSUPPORTED;
    }

    const AnvilBenchmarkService* c_api() const noexcept { return service_; }

private:
    static const AnvilBenchmarkService* lookup(const Host& host) noexcept {
        try {
            const auto* s =
                host.service<AnvilBenchmarkService>(ANVIL_BENCHMARK_SERVICE, ANVIL_BENCHMARK_SERVICE_VERSION);
            return s != nullptr && ANVIL_BENCHMARK_HAS(s, open_cost) ? s : nullptr;
        } catch (...) {
            return nullptr;  // used before the plugin was loading
        }
    }

    const AnvilBenchmarkService* service_ = nullptr;
    AnvilPlugin* self_ = nullptr;
};

}  // namespace anvil
