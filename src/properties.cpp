#include "detail.hpp"
#include <algorithm>
namespace magritte {
namespace {
void checkPropertyKey(const std::string &name, Limits limits) {
    if (name.empty() || name.size() > limits.bytes || name.size() >= limits.visits ||
        !std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Error("Invalid or over-budget property key");
}
void writable(const Description &d, const std::string &name) {
    if (name == "propertyAnnouncer" || d.properties.count("propertyAnnouncer"))
        throw Error("Property announcements are outside the stored-Value profile");
}
Value checked(Value value, Limits limits) {
    detail::checkLocalValue(value, limits);
    return value;
}
Value call(const std::function<Value()> &block, Limits limits) {
    if (!block)
        throw Error("Missing property fallback");
    return checked(block(), limits);
}
Value call(const std::function<Value(const Value &)> &block, const Value &value, Limits limits) {
    if (!block)
        throw Error("Missing property present callback");
    return checked(block(value), limits);
}
} // namespace
bool Description::hasProperty(const std::string &name) const {
    return properties.count(name) || (name == "displayBlockOrSymbol" && displayBlock);
}
Value Description::propertyAt(const std::string &name, Limits limits) const {
    checkPropertyKey(name, limits);
    if (name == "displayBlockOrSymbol" && displayBlock)
        throw Error("Display block has no stored Value representation");
    auto found = properties.find(name);
    if (found == properties.end())
        throw Error("MAPropertyError: property not found: " + name);
    return checked(found->second, limits);
}
Value Description::propertyAt(const std::string &name, const std::function<Value()> &fallback,
                              Limits limits) const {
    checkPropertyKey(name, limits);
    return hasProperty(name) ? propertyAt(name, limits) : call(fallback, limits);
}
Value Description::propertyAtIfAbsentPut(const std::string &name,
                                         const std::function<Value()> &fallback, Limits limits) {
    checkPropertyKey(name, limits);
    if (hasProperty(name))
        return propertyAt(name, limits);
    return propertyPut(name, call(fallback, limits), limits);
}
Value Description::propertyAtIfPresent(const std::string &name,
                                       const std::function<Value(const Value &)> &present,
                                       Limits limits) const {
    checkPropertyKey(name, limits);
    if (!hasProperty(name))
        return {};
    auto value = propertyAt(name, limits);
    return call(present, value, limits);
}
Value Description::propertyAtIfPresentOrElse(const std::string &name,
                                             const std::function<Value(const Value &)> &present,
                                             const std::function<Value()> &absent,
                                             Limits limits) const {
    checkPropertyKey(name, limits);
    if (!hasProperty(name))
        return call(absent, limits);
    auto value = propertyAt(name, limits);
    return call(present, value, limits);
}
Value Description::propertyAtIfPresentOrPut(const std::string &name,
                                            const std::function<Value(const Value &)> &present,
                                            const std::function<Value()> &absent, Limits limits) {
    checkPropertyKey(name, limits);
    if (!hasProperty(name))
        return propertyPut(name, call(absent, limits), limits);
    auto value = propertyAt(name, limits);
    return call(present, value, limits);
}
Value Description::propertyPut(const std::string &name, Value value, Limits limits) {
    checkPropertyKey(name, limits);
    writable(*this, name);
    value = checked(std::move(value), limits);
    properties[name] = value;
    if (name == "displayBlockOrSymbol")
        displayBlock.reset();
    return value;
}
Value Description::propertyPutRemovingNil(const std::string &name, Value value, Limits limits) {
    if (!value.isNil())
        return propertyPut(name, std::move(value), limits);
    checkPropertyKey(name, limits);
    writable(*this, name);
    if (!hasProperty(name))
        return {};
    auto old = propertyAt(name, limits);
    properties.erase(name);
    return old;
}
} // namespace magritte
