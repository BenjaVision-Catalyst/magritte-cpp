#include "detail.hpp"
namespace magritte {
namespace {
Value text(Value value) {
    if (!value.isNil() && !std::holds_alternative<std::string>(value.data))
        throw Error("Error message must be String or nil");
    return value;
}
Value message(const Description &d, const std::string &key, const std::string &fallback,
              Limits limits) {
    return text(d.propertyAt(key, [&] { return Value(fallback); }, limits));
}
void magnitude(const Description &d) {
    if (!ClassRegistry::global().includesBehavior(d.className, "MAMagnitudeDescription"))
        throw Error("Range message requires a magnitude description");
}
Description &put(Description &d, const std::string &key, Value value, Limits limits) {
    (void)d.propertyPut(key, text(std::move(value)), limits);
    return d;
}
} // namespace
Value Description::requiredErrorMessage(Limits limits) const {
    return message(*this, "requiredErrorMessage", "Input is required but no input given", limits);
}
Value Description::kindErrorMessage(Limits limits) const {
    return message(*this, "kindErrorMessage", "Invalid input given", limits);
}
Value Description::conflictErrorMessage(Limits limits) const {
    return message(*this, "conflictErrorMessage",
                   "Input is conflicting with concurrent modification", limits);
}
Value Description::rangeErrorMessage(Limits limits) const {
    magnitude(*this);
    return text(propertyAt(
        "rangeErrorMessage",
        [&] {
            // Upstream renders both bounds before inspecting their current presence.
            auto minText = writeString(*this, property("min"), nullptr, limits);
            auto maxText = writeString(*this, property("max"), nullptr, limits);
            bool hasMin = !property("min").isNil(), hasMax = !property("max").isNil();
            if (hasMin && hasMax)
                return Value("Input must be between " + minText + " and " + maxText);
            if (hasMin)
                return Value("Input must be above or equeal to " + minText);
            if (hasMax)
                return Value("Input must be below or equal to " + maxText);
            return Value();
        },
        limits));
}
Description &Description::setRequiredErrorMessage(Value v, Limits l) {
    return put(*this, "requiredErrorMessage", std::move(v), l);
}
Description &Description::setKindErrorMessage(Value v, Limits l) {
    return put(*this, "kindErrorMessage", std::move(v), l);
}
Description &Description::setConflictErrorMessage(Value v, Limits l) {
    return put(*this, "conflictErrorMessage", std::move(v), l);
}
Description &Description::setRangeErrorMessage(Value v, Limits l) {
    magnitude(*this);
    return put(*this, "rangeErrorMessage", std::move(v), l);
}
namespace detail {
std::optional<std::string> validationMessage(const Description &d, const std::string &code, Limits limits) {
    Value value;
    if (code == "MARequiredError")
        value = d.requiredErrorMessage(limits);
    else if (code == "MAKindError")
        value = d.kindErrorMessage(limits);
    else if (code == "MAConflictError")
        value = d.conflictErrorMessage(limits);
    else if (code == "MARangeError")
        value = d.rangeErrorMessage(limits);
    else
        throw Error("Unsupported description error message");
    auto s = std::get_if<std::string>(&value.data);
    if(value.isNil())return {};
    if(!s)throw Error("Error message must be String or nil");
    return *s;
}
} // namespace detail
} // namespace magritte
