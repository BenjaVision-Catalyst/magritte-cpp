#include "detail.hpp"
namespace magritte {
namespace detail {
std::shared_ptr<Description> referenceTarget(const Description &owner) {
    auto target = owner.reference.lock();
    if (!target)
        throw Error("Reference description is unavailable; retain it in the graph");
    return target;
}
} // namespace detail
std::shared_ptr<Description> Description::referenceDescription() const {
    auto target = detail::referenceTarget(*this);
    auto block = displayBlock;
    auto setting = properties.find("displayBlockOrSymbol");
    // Default inheritance clears an override without adding a new wire feature to legacy graphs.
    if (block)
        target->properties["displayBlockOrSymbol"] = Value();
    else if (setting != properties.end())
        target->properties["displayBlockOrSymbol"] = setting->second;
    else
        target->properties.erase("displayBlockOrSymbol");
    target->displayBlock = std::move(block);
    return target;
}
Description &Description::setUndefined(Value value, Limits limits) {
    if (!value.isNil() && !std::holds_alternative<std::string>(value.data))
        throw Error("Undefined text must be String or nil");
    detail::checkLocalValue(value, limits);
    std::size_t work = 0;
    std::function<void(Description &, std::size_t)> apply = [&](Description &current,
                                                                std::size_t depth) {
        if (++work > limits.visits || depth > limits.depth)
            throw Error("Undefined propagation work/depth limit");
        current.properties["undefined"] = value;
        if (current.isOption()) {
            // Upstream sends reference once for the nil test, then again for the setter.
            (void)current.referenceDescription();
            auto target = current.referenceDescription();
            apply(*target, depth + 1);
        }
    };
    apply(*this, 0);
    return *this;
}
} // namespace magritte
