#include "detail.hpp"
#include <algorithm>
#include <set>
namespace magritte {
std::vector<std::string> portableClasses() {
    static const std::vector<std::string> names{"Object",
                                                "UndefinedObject",
                                                "Boolean",
                                                "Number",
                                                "Integer",
                                                "Float",
                                                "String",
                                                "Symbol",
                                                "Character",
                                                "Collection",
                                                "SequenceableCollection",
                                                "Array",
                                                "OrderedCollection",
                                                "Set",
                                                "Bag",
                                                "Class",
                                                "ClassDescription",
                                                "Behavior",
                                                "MABasicObject",
                                                "MAAdaptiveModel",
                                                "MAObject",
                                                "MADescription",
                                                "MAElementDescription",
                                                "MAMagnitudeDescription",
                                                "MAReferenceDescription",
                                                "MARelationDescription",
                                                "MAOptionDescription",
                                                "MAContainer",
                                                "MAPriorityContainer",
                                                "MAStringDescription",
                                                "MASymbolDescription",
                                                "MAMemoDescription",
                                                "MAPasswordDescription",
                                                "MATokenDescription",
                                                "MANumberDescription",
                                                "MABooleanDescription",
                                                "MAClassDescription",
                                                "MAToOneRelationDescription",
                                                "MAToManyRelationDescription",
                                                "MAToManyScalarRelationDescription",
                                                "MASingleOptionDescription",
                                                "MAMultipleOptionDescription",
                                                "MAAccessor",
                                                "MADictionaryAccessor",
                                                "MAPropertyAccessor",
                                                "MASelectorAccessor",
                                                "MAVariableAccessor",
                                                "MANullAccessor",
                                                "MAIdentityAccessor",
                                                "MADelegatorAccessor",
                                                "MAChainAccessor",
                                                "MACondition",
                                                "MAPluggableCondition",
                                                "MAConjunctiveCondition",
                                                "MANegationCondition",
                                                "MAVisitor",
                                                "MAReader",
                                                "MAWriter",
                                                "MAStringReader",
                                                "MAStringWriter",
                                                "MAValidatorVisitor"};
    auto result = names;
    std::sort(result.begin(), result.end());
    return result;
}
namespace detail {
bool knownClass(const std::string &name) {
    static const auto classes = portableClasses();
    return std::binary_search(classes.begin(), classes.end(), name);
}
} // namespace detail
namespace {
void identity(const std::string &id, Limits l) {
    if (id.empty() || id.size() > l.bytes ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Error("Invalid reflection object identity");
}
} // namespace
std::shared_ptr<AccessorObject> AccessorObject::fromAccessor(const std::string &id, const Accessor &input,
                                                             Limits l) {
    input.check(l);
    std::size_t count = 0;
    std::function<std::shared_ptr<AccessorObject>(std::string, const Accessor &, std::size_t)> build =
        [&](auto key, const auto &a, auto depth) {
            if (++count > l.nodes || count > l.visits || depth > l.depth)
                throw Error("Accessor object construction limit");
            identity(key, l);
            auto n = std::make_shared<AccessorObject>();
            n->id = key;
            n->value = a;
            n->value.steps.clear();
            if (a.className == "MADelegatorAccessor")
                n->next = build(key + ".next", a.steps.at(0), depth + 1);
            else if (a.className == "MAChainAccessor") {
                // A binary chain matches MAChainAccessor's actual accessor/next slots.
                n->accessor = build(key + ".accessor", a.steps.at(0), depth + 1);
                auto tail = a.steps.size() == 2 ? a.steps.at(1) : a;
                if (a.steps.size() > 2)
                    tail.steps.assign(a.steps.begin() + 1, a.steps.end());
                n->next = build(key + ".next", tail, depth + 1);
            }
            return n;
        };
    return build(id, input, 0);
}
Accessor AccessorObject::snapshot(Limits l) const {
    std::set<const AccessorObject *> active;
    std::size_t count = 0;
    std::function<Accessor(const AccessorObject &, std::size_t)> make = [&](const auto &n, auto depth) {
        if (++count > l.visits || count > l.nodes || depth > l.depth)
            throw Error("Accessor object snapshot limit");
        identity(n.id, l);
        if (!active.insert(&n).second)
            throw Error("Accessor object cycle");
        auto a = n.value;
        if (!a.steps.empty())
            throw Error("Object accessor uses slots, not inline steps");
        if (a.className == "MAChainAccessor") {
            if (!n.accessor || !n.next)
                throw Error("Chain requires accessor and next");
            a.steps.push_back(make(*n.accessor, depth + 1));
            auto tail = make(*n.next, depth + 1);
            if (tail.className == "MAChainAccessor")
                a.steps.insert(a.steps.end(), tail.steps.begin(), tail.steps.end());
            else
                a.steps.push_back(tail);
        } else if (a.className == "MADelegatorAccessor") {
            if (n.accessor || !n.next)
                throw Error("Delegator requires only next");
            a.steps.push_back(make(*n.next, depth + 1));
        } else if (n.accessor || n.next)
            throw Error("Leaf accessor has unexpected slots");
        active.erase(&n);
        return a;
    };
    auto result = make(*this, 0);
    result.check(l);
    return result;
}
void Description::materializeAccessor(Limits l) {
    if (accessorObject) {
        auto value = accessorObject->snapshot(l);
        if (isContainer() && value.className == "MAIdentityAccessor")
            accessor.reset();
        else
            accessor = std::move(value);
        accessorObject.reset();
    }
}
} // namespace magritte
