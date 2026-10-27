#include "detail.hpp"
#include <set>
namespace magritte {
Value::Value(std::shared_ptr<CompositeConflict> v) : data(std::move(v)) {
    if (!std::get<std::shared_ptr<CompositeConflict>>(data))
        throw Error("Null conflict object");
}
std::shared_ptr<CompositeConflict> Value::conflict() const {
    auto p = std::get_if<std::shared_ptr<CompositeConflict>>(&data);
    if (p && !*p)
        throw Error("Null conflict object");
    return p ? *p : nullptr;
}
bool CompositeConflict::kindOf(const std::string &name) const {
    return (kind && *kind == name) ||
           ClassRegistry::global().includesBehavior("MACompositeConflict", name);
}
std::string CompositeConflict::doesNotUnderstand(
    const std::string &selector,
    const std::function<bool(const std::string &, const std::string &)> &understands) const {
    if (!kind || !understands || !understands(*kind, selector))
        throw Error("Message not understood: " + selector);
    return string();
}
Accessor Accessor::composite(Accessor head, Accessor next, std::optional<std::string> type) {
    Accessor result;
    result.className = "MACompositeAccessor";
    result.steps = {std::move(head), std::move(next)};
    result.kind = std::move(type);
    return result;
}
Accessor Accessor::compositeVia(Accessor head, Description &d, Limits limits) {
    auto type = d.property("kind", limits);
    if (!type.isNil() && type.data.index() != 10)
        throw Error("Description kind must be Class or nil");
    auto result = composite(std::move(head), d.effectiveAccessor(limits),
                            type.isNil() ? std::optional<std::string>{}
                                         : std::get<Value::Class>(type.data).name);
    result.check(limits);
    if (d.hasLabel(limits)) {
        auto label = result.steps.front().defaultLabelFor(d, limits) +
                     "::" + std::get<std::string>(d.property("label", limits).data);
        detail::checkLocalValue(Value(label), limits);
        d.properties["label"] = Value(label);
    }
    d.accessor = result;
    d.accessorObject.reset();
    return result;
}
namespace detail {
void checkLocalValue(const Value &value, Limits limits) {
    std::size_t work = 0;
    std::function<Value(const Value &, std::size_t)> project = [&](const Value &v,
                                                                   std::size_t depth) -> Value {
        if (++work > limits.visits || depth > limits.depth)
            throw Error("Local value work/depth limit");
        if (v.isMutableCollection()) {
          checkRuntimeValue(v, limits, work, depth);
          return Value();
        }
        if (auto c = v.conflict()) {
            if (c->kind && !ClassRegistry::global().find(*c->kind))
                throw Error("Unknown conflict kind");
            // The wire codec checks UTF-8, byte and item limits on the actual local state.
            Value::Array fields{Value(c->string())};
            if (c->kind)
                fields.push_back(Value(*c->kind));
            return Value(std::move(fields));
        }
        if (v.data.index() == 4 || v.data.index() == 7) {
            if (v.collectionSize() > limits.items)
                throw Error("Local value item limit");
            if (auto c = std::get_if<Value::Collection>(&v.data)) {
                if (c->kind != "set" && c->kind != "ordered" && c->kind != "bag")
                    throw Error("Unsupported collection kind");
                if (c->kind == "set") {
                    std::set<std::string> keys;
                    for (const auto &e : c->elements)
                        if (!keys.insert(valueKey(e, limits, work, depth + 1)).second)
                            throw Error("Duplicate set element");
                    Value::Array projected;
                    for (const auto &e : c->elements)
                        projected.push_back(project(e, depth + 1));
                    return Value(projected);
                }
            }
            auto n = v;
            auto &elements = n.data.index() == 4 ? std::get<Value::Array>(n.data)
                                                 : std::get<Value::Collection>(n.data).elements;
            for (auto &e : elements)
                e = project(e, depth + 1);
            return n;
        }
        return v;
    };
    (void)encodeValue(project(value, 0), limits);
}
} // namespace detail
} // namespace magritte
