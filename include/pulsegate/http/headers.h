#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulsegate::http {

struct Header {
    std::string name;
    std::string value;
};

class Headers {
   public:
    void add(std::string name, std::string value);
    void set(std::string name, std::string value);
    void erase(std::string_view name);

    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const;
    [[nodiscard]] std::vector<std::string_view> values(std::string_view name) const;
    [[nodiscard]] const std::vector<Header>& entries() const noexcept;

   private:
    std::vector<Header> entries_;
};

[[nodiscard]] std::string normalizeHeaderName(std::string_view name);
[[nodiscard]] bool isValidHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool containsToken(std::string_view value, std::string_view token) noexcept;

}  // namespace pulsegate::http
