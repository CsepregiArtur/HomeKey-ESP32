#pragma once

#include <cJSON.h>

#include <concepts>
#include <cstdlib>
#include <expected>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief RAII wrapper for cJSON objects.
 */
class JsonGuard {
    cJSON* ptr_;

public:
    JsonGuard() noexcept : ptr_(nullptr) {}
    JsonGuard(std::nullptr_t) noexcept : ptr_(nullptr) {}
    
    JsonGuard(cJSON* p) noexcept : ptr_(p) {}
    
    ~JsonGuard() noexcept {
        if (ptr_) cJSON_Delete(ptr_);
    }
    
    JsonGuard(const JsonGuard&) = delete;
    JsonGuard& operator=(const JsonGuard&) = delete;
    
    JsonGuard(JsonGuard&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    
    JsonGuard& operator=(JsonGuard&& other) noexcept {
        if (this != &other) {
            if (ptr_) cJSON_Delete(ptr_);
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    
    [[nodiscard]] cJSON* get() const noexcept { return ptr_; }
    
    [[nodiscard("Leaks managed cJSON object")]] cJSON* release() noexcept {
        cJSON* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }
    
    void reset(cJSON* p = nullptr) noexcept {
        if (ptr_) cJSON_Delete(ptr_);
        ptr_ = p;
    }
    
    void swap(JsonGuard& other) noexcept {
        cJSON* tmp = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = tmp;
    }
    
    [[nodiscard]] explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }
};

namespace detail {
    inline std::string print_json(const cJSON* obj, char* (*printer)(const cJSON*)) {
        if (!obj) return "";
        char* raw = printer(obj);
        if (!raw) return "";
        std::string result = raw;
        cJSON_free(raw);
        return result;
    }
}

[[nodiscard]] inline std::string to_string_unformatted(const JsonGuard& g) {
    return detail::print_json(g.get(), cJSON_PrintUnformatted);
}

[[nodiscard]] inline std::string to_string_formatted(const JsonGuard& g) {
    return detail::print_json(g.get(), cJSON_Print);
}

[[nodiscard]] inline std::string to_string_and_free(JsonGuard g) {
    return to_string_unformatted(g);
}

class JsonArrayView {
    const cJSON* head_;

public:
    class iterator {
        const cJSON* current_;
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const cJSON*;
        using difference_type = std::ptrdiff_t;
        using pointer = const cJSON* const*;
        using reference = const cJSON*;

        explicit iterator(const cJSON* start) noexcept : current_(start) {}
        reference operator*() const noexcept { return current_; }
        iterator& operator++() noexcept { if (current_) current_ = current_->next; return *this; }
        iterator operator++(int) noexcept { iterator tmp = *this; ++(*this); return tmp; }
        bool operator==(const iterator& other) const noexcept = default;
    };

    explicit JsonArrayView(const cJSON* arr) noexcept : head_(arr ? arr->child : nullptr) {}
    iterator begin() const noexcept { return iterator(head_); }
    iterator end() const noexcept { return iterator(nullptr); }
};

[[nodiscard]] inline JsonArrayView as_array(const JsonGuard& g) noexcept {
    return JsonArrayView(g.get());
}

[[nodiscard]] inline JsonArrayView as_array(const cJSON* obj) noexcept {
    return JsonArrayView(obj);
}

struct JsonParseError {
    std::string message;
};

[[nodiscard]] inline std::expected<JsonGuard, JsonParseError> parse_json(std::string_view json_str) {
    std::string str(json_str);
    cJSON* ptr = cJSON_Parse(str.c_str());
    if (!ptr) {
        const char* err = cJSON_GetErrorPtr();
        return std::unexpected(JsonParseError{ err ? std::string(err) : "Unknown parse error" });
    }
    return JsonGuard(ptr);
}

class JsonBuilder {
    JsonGuard guard_;

    explicit JsonBuilder(JsonGuard g) noexcept : guard_(std::move(g)) {}

public:
    JsonBuilder(JsonBuilder&&) noexcept = default;
    JsonBuilder& operator=(JsonBuilder&&) noexcept = default;
    JsonBuilder(const JsonBuilder&) = delete;
    JsonBuilder& operator=(const JsonBuilder&) = delete;

    [[nodiscard]] static JsonBuilder object() noexcept {
        return JsonBuilder{JsonGuard(cJSON_CreateObject())};
    }
    
    [[nodiscard]] static JsonBuilder array() noexcept {
        return JsonBuilder{JsonGuard(cJSON_CreateArray())};
    }

    [[nodiscard]] cJSON* get() const noexcept { return guard_.get(); }
    [[nodiscard]] JsonGuard extractGuard() noexcept { return std::move(guard_); }
    
    [[nodiscard("Leaks managed cJSON object")]] cJSON* release() noexcept {
        return guard_.release();
    }
    
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(guard_);
    }

    [[nodiscard]] std::string toStringUnformatted() const {
        return to_string_unformatted(guard_);
    }
    
    [[nodiscard]] std::string toStringFormatted() const {
        return to_string_formatted(guard_);
    }

    JsonBuilder& addString(const char* key, const char* value) noexcept {
        if (guard_ && key && value) cJSON_AddStringToObject(guard_.get(), key, value);
        return *this;
    }

    JsonBuilder& addString(std::string_view key, std::string_view value) {
        if (guard_) {
            std::string k(key);
            std::string v(value);
            cJSON_AddStringToObject(guard_.get(), k.c_str(), v.c_str());
        }
        return *this;
    }

    JsonBuilder& addBool(const char* key, bool value) noexcept {
        if (guard_ && key) cJSON_AddBoolToObject(guard_.get(), key, value ? 1 : 0);
        return *this;
    }

    JsonBuilder& addBool(std::string_view key, bool value) {
        if (guard_) {
            std::string k(key);
            cJSON_AddBoolToObject(guard_.get(), k.c_str(), value ? 1 : 0);
        }
        return *this;
    }

    JsonBuilder& addNumber(const char* key, double value) noexcept {
        if (guard_ && key) cJSON_AddNumberToObject(guard_.get(), key, value);
        return *this;
    }

    JsonBuilder& addNumber(std::string_view key, double value) {
        if (guard_) {
            std::string k(key);
            cJSON_AddNumberToObject(guard_.get(), k.c_str(), value);
        }
        return *this;
    }

    JsonBuilder& addItem(const char* key, JsonGuard item) noexcept {
        if (guard_ && key && item) {
            cJSON* raw = item.get();
            if (cJSON_AddItemToObject(guard_.get(), key, raw)) {
                (void)item.release(); 
            }
        }
        return *this;
    }

    JsonBuilder& addItemToArray(JsonGuard item) noexcept {
        if (guard_ && item) {
            cJSON* raw = item.get();
            if (cJSON_AddItemToArray(guard_.get(), raw)) {
                (void)item.release();
            }
        }
        return *this;
    }

    JsonBuilder& withObject(const char* key, std::invocable<JsonBuilder&> auto&& fill) noexcept {
        if (guard_ && key) {
            auto sub = JsonBuilder::object();
            if (sub) {
                fill(sub);
                addItem(key, sub.extractGuard());
            }
        }
        return *this;
    }

    JsonBuilder& withArray(const char* key, std::invocable<JsonBuilder&> auto&& fill) noexcept {
        if (guard_ && key) {
            auto sub = JsonBuilder::array();
            if (sub) {
                fill(sub);
                addItem(key, sub.extractGuard());
            }
        }
        return *this;
    }
};
