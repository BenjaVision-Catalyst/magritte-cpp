#include "detail.hpp"
namespace magritte {
namespace {
void element(const Description &d) {
    if (!ClassRegistry::global().includesBehavior(d.className, "MAElementDescription"))
        throw Error("Lazy/initial answer protocol requires an element description");
}
Value declaredDefault(const Description &d, Limits limits) {
    if (d.defaultFunction)
        throw Error("Lazy initialization/initial answer does not evaluate callable defaults");
    auto value = d.property("default", limits);
    detail::checkLocalValue(value, limits);
    return value;
}
} // namespace
Value Description::initialAnswer(Limits limits) const {
    element(*this);
    return propertyAt("initialAnswer", [&] { return declaredDefault(*this, limits); }, limits);
}
Description &Description::setInitialAnswer(Value value, Limits limits) {
    element(*this);
    (void)propertyPut("initialAnswer", std::move(value), limits);
    return *this;
}
Value Description::lazilyInitializeFrom(const Value &current, Model &model, Limits limits) const {
    element(*this);
    detail::checkLocalValue(current, limits);
    if (!current.isNil())
        return current;
    auto value = declaredDefault(*this, limits);
    if (flag("shouldCacheDefault"))
        model.writeUsing(*this, value);
    return value;
}
Value Description::maLazyPropertyUsing(const Description &description, Limits limits) {
    auto accessor = description.effectiveAccessor(limits);
    if (accessor.className != "MASelectorAccessor" || accessor.name.empty())
        throw Error("maLazyPropertyUsing requires a nonempty selector read accessor");
    auto current = propertyAt(accessor.name, [] { return Value(); }, limits);
    return description.lazilyInitializeFrom(current, *this, limits);
}
} // namespace magritte
