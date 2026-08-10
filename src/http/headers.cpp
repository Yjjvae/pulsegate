#include "pulsegate/http/headers.h"

#include <algorithm>
#include <cctype>

namespace pulsegate::http {
namespace {

bool equalsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs)) ==
                      std::tolower(static_cast<unsigned char>(rhs));
           });
}

bool isTokenCharacter(char character) noexcept {
    const auto value = static_cast<unsigned char>(character);
    if (std::isalnum(value) != 0) {
        return true;
    }
    constexpr std::string_view kTokenPunctuation{"!#$%&'*+-.^_`|~"};
    return kTokenPunctuation.find(character) != std::string_view::npos;
}

}  // namespace

void Headers::add(std::string name, std::string value) {
    entries_.push_back({normalizeHeaderName(name), std::move(value)});
}

void Headers::set(std::string name, std::string value) {
    erase(name);
    add(std::move(name), std::move(value));
}

void Headers::erase(std::string_view name) {
    const auto normalized = normalizeHeaderName(name);
    std::erase_if(entries_,
                  [&normalized](const Header& header) { return header.name == normalized; });
}

bool Headers::contains(std::string_view name) const {
    return get(name).has_value();
}

std::optional<std::string_view> Headers::get(std::string_view name) const {
    const auto normalized = normalizeHeaderName(name);
    const auto found =
        std::find_if(entries_.begin(), entries_.end(),
                     [&normalized](const Header& header) { return header.name == normalized; });
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->value;
}

std::vector<std::string_view> Headers::values(std::string_view name) const {
    const auto normalized = normalizeHeaderName(name);
    std::vector<std::string_view> result;
    for (const auto& header : entries_) {
        if (header.name == normalized) {
            result.push_back(header.value);
        }
    }
    return result;
}

const std::vector<Header>& Headers::entries() const noexcept {
    return entries_;
}

std::string normalizeHeaderName(std::string_view name) {
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return normalized;
}

bool isValidHeaderName(std::string_view name) noexcept {
    return !name.empty() && std::all_of(name.begin(), name.end(), isTokenCharacter);
}

bool isValidHeaderValue(std::string_view value) noexcept {
    return std::none_of(value.begin(), value.end(), [](char character) {
        return character == '\r' || character == '\n' || character == '\0';
    });
}

bool containsToken(std::string_view value, std::string_view token) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto part = value.substr(0, comma);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
            part.remove_prefix(1);
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
            part.remove_suffix(1);
        }
        if (equalsIgnoreCase(part, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

}  // namespace pulsegate::http
