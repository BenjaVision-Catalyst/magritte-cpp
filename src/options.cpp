#include <magritte/magritte.hpp>
#include "detail.hpp"

namespace magritte {
namespace {
void bounded(const Value& value, Limits limits) { (void)encodeValue(value, limits); }
}
Value DynamicOptions::selectMatching(const std::string& query, Limits limits) const {
    bounded(Value(query), limits);
    auto callback = block_;
    if (!callback) throw Error("Dynamic options completion block is missing");
    auto result = (*callback)(query);
    bounded(result, limits);
    return result;
}
bool DynamicOptions::includes(const Value& value, Limits limits) const {
    bounded(value, limits);
    auto callback = includes_;
    if (!callback) throw Error("Dynamic options includes block is missing");
    return (*callback)(value);
}
Value DynamicOptions::detect(const Value& value, Limits limits) const {
    bounded(value, limits);
    auto callback = reader_;
    auto result = callback ? (*callback)(value) : value;
    bounded(result, limits);
    return result;
}
std::shared_ptr<DynamicOptions> DynamicOptions::copy() const {
    return std::make_shared<DynamicOptions>(*this);
}
Options::Options(std::vector<Value> values) {
    std::vector<OptionItem> entries;
    for (auto& value : values) entries.emplace_back(std::move(value));
    data_ = std::move(entries);
}
Options::Options(std::shared_ptr<DynamicOptions> provider) : data_(std::move(provider)) {
    if (!std::get<std::shared_ptr<DynamicOptions>>(data_)) throw Error("Null dynamic options provider");
}
Options Options::collection(std::vector<OptionItem> entries) {
    for (const auto& entry : entries)
        if (auto provider = std::get_if<std::shared_ptr<DynamicOptions>>(&entry); provider && !*provider)
            throw Error("Null provider entry");
    Options result;
    result.data_ = std::move(entries);
    return result;
}
bool Options::isDynamic() const { return std::holds_alternative<std::shared_ptr<DynamicOptions>>(data_); }
std::shared_ptr<DynamicOptions> Options::provider() const {
    if (!isDynamic()) throw Error("Options are a collection, not a provider");
    auto result = std::get<std::shared_ptr<DynamicOptions>>(data_);
    if (!result) throw Error("Null dynamic options provider");
    return result;
}
const std::vector<OptionItem>& Options::entries() const {
    if (isDynamic()) throw Error("Dynamic options are not an enumerable collection");
    return std::get<std::vector<OptionItem>>(data_);
}
std::vector<Value> Options::values(Limits limits) const {
    if (size() > limits.items) throw Error("Options item limit");
    std::vector<Value> result;
    for (const auto& entry : entries()) {
        auto value = std::get_if<Value>(&entry);
        if (!value) throw Error("Local dynamic provider entry is not a portable Value");
        result.push_back(*value);
    }
    return result;
}
void Options::push_back(Value value) {
    (void)entries();
    std::get<std::vector<OptionItem>>(data_).emplace_back(std::move(value));
}
void Options::pop_back() {
    if (entries().empty()) throw Error("Empty options collection");
    std::get<std::vector<OptionItem>>(data_).pop_back();
}
Options Options::copy() const { return isDynamic() ? Options(provider()->copy()) : *this; }
Value Options::selectMatching(const std::string& query, Limits limits) const {
    if (isDynamic()) return provider()->selectMatching(query, limits);
    bounded(Value(query), limits);
    auto source = values(limits);
    std::size_t work = 0;
    Value::Array result;
    for (const auto& value : source) {
        (void)detail::valueKey(value, limits, work);
        auto text = detail::displayString(value);
        if (text.rfind(query, 0) == 0) result.push_back(value);
    }
    Value value(std::move(result)); bounded(value, limits); return value;
}
bool Options::operator==(const Options& other) const {
    if (isDynamic() != other.isDynamic()) return false;
    if (isDynamic()) return provider() == other.provider();
    if (size() != other.size()) return false;
    for (std::size_t i = 0; i < size(); ++i) {
        auto a = std::get_if<Value>(&entries()[i]), b = std::get_if<Value>(&other.entries()[i]);
        if (bool(a) != bool(b)) return false;
        if (a) { if (!(*a == *b)) return false; }
        else if (std::get<std::shared_ptr<DynamicOptions>>(entries()[i]) != std::get<std::shared_ptr<DynamicOptions>>(other.entries()[i])) return false;
    }
    return true;
}
} // namespace magritte
