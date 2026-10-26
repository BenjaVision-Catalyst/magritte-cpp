#include "detail.hpp"
#include <magritte/magritte.hpp>
#include <set>

namespace magritte {
namespace detail {
std::string descriptionType(const Description &d) {
    (void)descriptionImplementation(d);
    return ClassRegistry::global().find(d.className)->label;
}
bool metadataSelector(const Description &d, const std::string &name, bool writing) {
    static const std::set<std::string> common{"label",          "comment",           "group",    "required",
                                              "readOnly",       "visible",           "priority", "undefined",
                                              "undefinedValue", "shouldCacheDefault", "stringReader", "stringWriter", "validator"};
    if (name == "requiredErrorMessage" || name == "kindErrorMessage" ||
        name == "conflictErrorMessage")
        return true;
    if (name == "rangeErrorMessage")
        return ClassRegistry::global().includesBehavior(d.className, "MAMagnitudeDescription");
    if (common.count(name))
        return true;
    if (name == "initialAnswer")
        return !d.isContainer();
    if (name == "name")
        return true;
    if (name == "defaultLabel" || name == "hasLabel" || name == "isSortable")
        return !writing;
    if (name == "type")
        return !writing;
    if (name == "default")
        return !writing || !d.isContainer();
    if ((name == "min" || name == "max" || name == "thousandsSeparator") &&
        detail::descriptionIs(d, "MANumberDescription"))
        return true;
    if ((name == "trueString" || name == "falseString") && detail::descriptionIs(d, "MABooleanDescription"))
        return true;
    if (name == "lineCount" && detail::descriptionIs(d, "MAMemoDescription"))
        return true;
    if (name == "separators" && detail::descriptionIs(d, "MATokenDescription"))
        return true;
    if ((name == "sorted" || name == "extensible") && d.isOption())
        return true;
    if (name == "distinct" && detail::descriptionIs(d, "MAMultipleOptionDescription"))
        return true;
    if ((name == "ordered" || name == "definitive" || name == "sorted") && d.isRelation() && d.isMultiple())
        return true;
    return name == "ordered" && detail::descriptionIs(d, "MAMultipleOptionDescription");
}
} // namespace detail
Graph descriptionModelFor(const Description &target, Limits limits) {
    (void)detail::descriptionType(target);
    Graph graph;
    if (target.className != detail::descriptionImplementation(target) ||
        detail::descriptionIs(target, "MAClassDescription"))
        graph.format = "magritte.graph/4";
    graph.root = "meta." + target.className;
    auto root = std::make_shared<Description>();
    root->id = graph.root;
    root->className = "MAContainer";
    root->properties["label"] = Value(target.className);
    graph.descriptions.emplace(root->id, root);
    auto add = [&](std::string key, std::string cls, std::string label, std::int64_t priority,
                   std::optional<Value> defaultValue = std::nullopt) {
        auto d = std::make_shared<Description>();
        d->id = graph.root + "." + key;
        d->className = std::move(cls);
        Accessor a;
        a.className = "MASelectorAccessor";
        a.name = key;
        a.writeName = key + ":";
        d->accessor = a;
        if (!label.empty())
            d->properties["label"] = Value(std::move(label));
        d->properties["priority"] = Value(priority);
        if (defaultValue)
            d->properties["default"] = *defaultValue;
        if (d->isReference()) {
            auto ref = std::make_shared<Description>();
            ref->id = d->id + ".reference";
            Accessor null;
            null.className = "MANullAccessor";
            ref->accessor = null;
            d->reference = ref;
            graph.descriptions.emplace(ref->id, ref);
            if (detail::descriptionIs(*d, "MATokenDescription"))
                d->properties["separators"] = d->property("separators");
        }
        root->children.push_back(d);
        graph.descriptions.emplace(d->id, d);
        return d;
    };
    auto type = add("type", "MAStringDescription", "Kind", 0);
    type->properties["readOnly"] = Value(true);
    add("label", "MAStringDescription", "Label", 100, Value(target.defaultLabel(limits)));
    add("comment", "MAMemoDescription", "Comment", 110, Value());
    add("group", "MAStringDescription", "Group", 105, Value());
    auto priority = add("priority", "MANumberDescription", "Priority", 130, Value(std::int64_t(0)));
    priority->properties["required"] = Value(true);
    add("readOnly", "MABooleanDescription", "Read-only", 200, Value(false));
    if (!detail::descriptionIs(target, "MABooleanDescription"))
        add("required", "MABooleanDescription", "Required", 220, Value(false));
    add("visible", "MABooleanDescription", "Visible", 210, Value(true));
    add("undefined", "MAStringDescription", "Undefined String", 140, Value(""));
    add("undefinedValue", "MAToOneRelationDescription", "", 150, Value());
    if (!target.isContainer() && !target.isOption())
        add("default", target.className, "Default", 130, Value());
    if (detail::descriptionIs(target, "MANumberDescription")) {
        add("min", target.className, "Min", 400);
        add("max", target.className, "Maximum", 410);
    }
    if (detail::descriptionIs(target, "MABooleanDescription")) {
        add("trueString", "MAStringDescription", "True String", 400, Value("true"));
        add("falseString", "MAStringDescription", "False String", 410, Value("false"));
    }
    if (detail::descriptionIs(target, "MAMemoDescription")) {
        auto lines = add("lineCount", "MANumberDescription", "Number of Lines", 400, Value(std::int64_t(3)));
        lines->properties["min"] = Value(std::int64_t(1));
        Predicate integer;
        integer.selector = "isInteger";
        lines->portableConditions.push_back({"No integer was entered", integer});
    }
    if (detail::descriptionIs(target, "MATokenDescription"))
        add("separators", "MAStringDescription", "Separators", 250);
    if (target.isOption()) {
        add("sorted", "MABooleanDescription", "Sorted", 240, Value(false));
        add("extensible", "MABooleanDescription", "Extensible", 250, Value(false));
    }
    if (target.isRelation() && target.isMultiple()) {
        add("ordered", "MABooleanDescription", "Ordered", 260, Value(false));
        add("definitive", "MABooleanDescription", "Definitive", 265, Value(false));
        add("sorted", "MABooleanDescription", "Sorted", 240, Value(false));
    }
    if (detail::descriptionIs(target, "MAMultipleOptionDescription")) {
        add("distinct", "MABooleanDescription", "Distinct", 250, Value(false));
        add("ordered", "MABooleanDescription", "Ordered", 260, Value(false));
    }
    (void)graph.encode(limits);
    return graph;
}
} // namespace magritte
