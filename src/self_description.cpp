#include "detail.hpp"
#include <magritte/magritte.hpp>
#include <set>

namespace magritte {
namespace detail {
std::string descriptionType(const Description &d) {
    static const std::map<std::string, std::string> names{
        {"MAContainer", "Container"},
        {"MAPriorityContainer", "Priority Container"},
        {"MAStringDescription", "String"},
        {"MASymbolDescription", "Symbol"},
        {"MAMemoDescription", "Memo"},
        {"MAPasswordDescription", "Password"},
        {"MANumberDescription", "Number"},
        {"MABooleanDescription", "Boolean"},
        {"MATokenDescription", "Token"},
        {"MAToOneRelationDescription", "1:1 Relation"},
        {"MAToManyRelationDescription", "1:m Relation"},
        {"MAToManyScalarRelationDescription", "1:m scalar Relation"},
        {"MASingleOptionDescription", "Single-Option"},
        {"MAMultipleOptionDescription", "Multiple-Option"}};
    auto i = names.find(d.className);
    if (i == names.end())
        throw Error("Unsupported self-description class");
    return i->second;
}
bool metadataSelector(const Description &d, const std::string &name, bool writing) {
    static const std::set<std::string> common{"label",          "comment",           "group",    "required",
                                              "readOnly",       "visible",           "priority", "undefined",
                                              "undefinedValue", "shouldCacheDefault"};
    if (common.count(name))
        return true;
    if (name == "type")
        return !writing;
    if (name == "default")
        return !writing || !d.isContainer();
    if ((name == "min" || name == "max" || name == "thousandsSeparator") &&
        d.className == "MANumberDescription")
        return true;
    if ((name == "trueString" || name == "falseString") && d.className == "MABooleanDescription")
        return true;
    if (name == "lineCount" && d.className == "MAMemoDescription")
        return true;
    if (name == "separators" && d.className == "MATokenDescription")
        return true;
    if ((name == "sorted" || name == "extensible") && d.isOption())
        return true;
    if (name == "distinct" && d.className == "MAMultipleOptionDescription")
        return true;
    if ((name == "ordered" || name == "definitive" || name == "sorted") && d.isRelation() && d.isMultiple())
        return true;
    return name == "ordered" && d.className == "MAMultipleOptionDescription";
}
} // namespace detail
Graph descriptionModelFor(const Description &target, Limits limits) {
    (void)detail::descriptionType(target);
    Graph graph;
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
            if (d->className == "MATokenDescription")
                d->properties["separators"] = d->property("separators");
        }
        root->children.push_back(d);
        graph.descriptions.emplace(d->id, d);
        return d;
    };
    auto type = add("type", "MAStringDescription", "Kind", 0);
    type->properties["readOnly"] = Value(true);
    auto fallback = target;
    fallback.properties.erase("label");
    add("label", "MAStringDescription", "Label", 100, fallback.property("label"));
    add("comment", "MAMemoDescription", "Comment", 110, Value());
    add("group", "MAStringDescription", "Group", 105, Value());
    auto priority = add("priority", "MANumberDescription", "Priority", 130, Value(std::int64_t(0)));
    priority->properties["required"] = Value(true);
    add("readOnly", "MABooleanDescription", "Read-only", 200, Value(false));
    if (target.className != "MABooleanDescription")
        add("required", "MABooleanDescription", "Required", 220, Value(false));
    add("visible", "MABooleanDescription", "Visible", 210, Value(true));
    add("undefined", "MAStringDescription", "Undefined String", 140, Value(""));
    add("undefinedValue", "MAToOneRelationDescription", "", 150, Value());
    if (!target.isContainer() && !target.isOption())
        add("default", target.className, "Default", 130, Value());
    if (target.className == "MANumberDescription") {
        add("min", target.className, "Min", 400);
        add("max", target.className, "Maximum", 410);
    }
    if (target.className == "MABooleanDescription") {
        add("trueString", "MAStringDescription", "True String", 400, Value("true"));
        add("falseString", "MAStringDescription", "False String", 410, Value("false"));
    }
    if (target.className == "MAMemoDescription") {
        auto lines = add("lineCount", "MANumberDescription", "Number of Lines", 400, Value(std::int64_t(3)));
        lines->properties["min"] = Value(std::int64_t(1));
        Predicate integer;
        integer.selector = "isInteger";
        lines->portableConditions.push_back({"No integer was entered", integer});
    }
    if (target.className == "MATokenDescription")
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
    if (target.className == "MAMultipleOptionDescription") {
        add("distinct", "MABooleanDescription", "Distinct", 250, Value(false));
        add("ordered", "MABooleanDescription", "Ordered", 260, Value(false));
    }
    (void)graph.encode(limits);
    return graph;
}
} // namespace magritte
