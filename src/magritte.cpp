#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include "detail.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace magritte {
namespace {
using J = nlohmann::json;
void require(bool ok, const std::string& message) { if (!ok) throw Error(message); }
void keys(const J& j, std::initializer_list<const char*> expected) {
    require(j.is_object(), "Expected object");
    std::set<std::string> names;
    for (auto key : expected) names.insert(key);
    require(j.size() == names.size(), "Missing or unsupported members");
    for (auto it = j.begin(); it != j.end(); ++it)
        require(names.count(it.key()), "Unsupported member: " + it.key());
}
std::string string(const J& j) { require(j.is_string(), "Expected string"); return j.get<std::string>(); }
std::string identity(const J& j) { auto s = string(j); require(!s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 33 && c <= 126; }), "Expected nonempty ASCII identity"); return s; }
J parse(const std::string& input, Limits l) {
    require(input.size() <= l.bytes, "Input byte limit");
    std::vector<std::set<std::string>> objects;
    std::size_t items = 0;
    return J::parse(input, [&](int depth, J::parse_event_t event, J& value) {
        require(depth >= 0 && static_cast<std::size_t>(depth) <= l.depth, "JSON depth limit");
        require(++items <= l.items, "JSON item limit");
        if (event == J::parse_event_t::object_start) objects.emplace_back();
        if (event == J::parse_event_t::key)
            require(objects.back().insert(value.get<std::string>()).second, "Duplicate JSON key");
        if (event == J::parse_event_t::object_end) objects.pop_back();
        return true;
    });
}
Value value(const J& j, Limits l, std::size_t depth = 0) {
    require(depth <= l.depth, "Value depth limit");
    require(j.is_object() && j.contains("type"), "Expected tagged value");
    const auto type = string(j.at("type"));
    if (type == "nil") { keys(j, {"type"}); return {}; }
    keys(j, {"type", "value"});
    const auto& v = j.at("value");
    if (type == "fraction") {
        keys(v, {"numerator", "denominator"});
        auto n = value(J{{"type", "integer"}, {"value", v.at("numerator")}}, l, depth + 1);
        auto d = value(J{{"type", "integer"}, {"value", v.at("denominator")}}, l, depth + 1);
        return Value(
            Value::Fraction{std::get<std::int64_t>(n.data), std::get<std::int64_t>(d.data)});
    }
    if (type == "string") return Value(string(v));
    if (type == "class") return Value(Value::Class{identity(v)});
    if (type == "symbol") return Value(Value::Symbol{string(v)});
    if (type == "float") return Value(detail::floatFromBits(string(v)));
    if (type == "reference") return Value(Value::Reference{identity(v)});
    if (type == "character") {
        auto text = string(v); auto chars = Value(text).elements();
        require(chars.size() == 1, "Character requires one Unicode scalar");
        return chars.front();
    }
    if (type == "boolean") { require(v.is_boolean(), "Expected Boolean"); return Value(v.get<bool>()); }
    if (type == "integer") {
        auto s = string(v);
        std::int64_t i{};
        auto r = std::from_chars(s.data(), s.data() + s.size(), i);
        require(r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::to_string(i) == s,
                "Unsupported or noncanonical integer");
        return Value(i);
    }
    if (type == "array" || type == "ordered" || type == "set" || type == "bag") {
        require(v.is_array() && v.size() <= l.items, "Expected bounded array");
        Value::Array a;
        for (const auto& e : v) a.push_back(value(e, l, depth + 1));
        if (type == "set") {
            std::size_t work = 0;
            std::set<std::string> keys;
            for (const auto& e : a) require(keys.insert(detail::valueKey(e, l, work)).second, "Duplicate set element");
        }
        return type == "array" ? Value(std::move(a)) : Value(Value::Collection{type, std::move(a)});
    }
    throw Error("Unsupported value type: " + type);
}
J wire(const Value& v, Limits l, std::size_t depth = 0) {
    require(depth <= l.depth, "Value depth limit");
    switch (v.data.index()) {
    case 12: {
        auto f = detail::fractionOf(v);
        return J{{"type", "fraction"},
                 {"value", J{{"numerator", std::to_string(f.numerator)},
                             {"denominator", std::to_string(f.denominator)}}}};
    }
    case 11:
        throw Error("Composite conflicts are local objects and cannot be exported");
    case 0:
        return J{{"type", "nil"}};
    case 1:
        return J{{"type", "boolean"}, {"value", std::get<bool>(v.data)}};
    case 2:
        return J{{"type", "integer"}, {"value", std::to_string(std::get<std::int64_t>(v.data))}};
    case 3:
        return J{{"type", "string"}, {"value", std::get<std::string>(v.data)}};
    case 8:
        return J{{"type", "symbol"}, {"value", std::get<Value::Symbol>(v.data).utf8}};
    case 9:
        return J{{"type", "float"}, {"value", detail::floatBits(std::get<double>(v.data))}};
    case 10: {
        auto name = std::get<Value::Class>(v.data).name;
        require(detail::knownClass(name), "Unknown portable class");
        return J{{"type", "class"}, {"value", name}};
    }
        case 5: return J{{"type", "reference"}, {"value", identity(J(std::get<Value::Reference>(v.data).id))}};
        case 6: {
            auto text = std::get<Value::Character>(v.data).utf8;
            require(Value(text).elements().size() == 1, "Character requires one Unicode scalar");
            return J{{"type", "character"}, {"value", text}};
        }
        default: {
            auto a = J::array();
            auto type = v.data.index() == 4 ? "array" : std::get<Value::Collection>(v.data).kind;
            require(type == "array" || type == "ordered" || type == "set" || type == "bag", "Unsupported collection kind");
            const auto& values = v.data.index() == 4 ? std::get<Value::Array>(v.data) : std::get<Value::Collection>(v.data).elements;
            require(values.size() <= l.items, "Value item limit");
            for (const auto& e : values) a.push_back(wire(e, l, depth + 1));
            return J{{"type", type}, {"value", a}};
        }
    }
}
J predicateWire(const Predicate &p, Limits l, std::size_t depth = 0) {
    require(depth <= l.depth, "Condition depth limit");
    if (p.className == "MAPluggableCondition") {
        J args = J::array();
        for (const auto &v : p.arguments)
            args.push_back(wire(v, l));
        return J{{"class", p.className},
                 {"selector", p.selector},
                 {"receiver", wire(p.receiver, l)},
                 {"arguments", args},
                 {"index", p.index}};
    }
    J a = J::array();
    for (const auto &c : p.children)
        a.push_back(predicateWire(c, l, depth + 1));
    return J{{"class", p.className}, {"conditions", a}};
}
Predicate predicateValue(const J &j, Limits l, std::size_t depth = 0) {
    require(depth <= l.depth, "Condition depth limit");
    Predicate p;
    p.className = string(j.at("class"));
    if (p.className == "MAPluggableCondition") {
        keys(j, {"class", "selector", "receiver", "arguments", "index"});
        p.selector = string(j.at("selector"));
        p.receiver = value(j.at("receiver"), l);
        require(j.at("index").is_number() && j.at("index") >= 0 && j.at("index") <= 2 &&
                    std::floor(j.at("index").get<double>()) == j.at("index").get<double>(),
                "Expected condition index");
        p.index = j.at("index").get<std::size_t>();
        require(j.at("arguments").is_array(), "Expected condition arguments");
        for (const auto &v : j.at("arguments"))
            p.arguments.push_back(value(v, l));
    } else {
        keys(j, {"class", "conditions"});
        require(j.at("conditions").is_array(), "Expected condition children");
        for (const auto &c : j.at("conditions"))
            p.children.push_back(predicateValue(c, l, depth + 1));
    }
    p.check(l);
    return p;
}
J accessorWire(const Accessor &a, Limits l, std::size_t depth = 0) {
    require(depth <= l.depth, "Accessor depth limit");
    require(a.className != "MAPluggableAccessor",
            "Pluggable accessor blocks are local and cannot be exported");
    J j{{"class", a.className}};
    if (a.className == "MANullAccessor")
        j["uuid"] = wire(a.uuid, l);
    else if(a.className=="MADelegatorAccessor")return J{{"class",a.className},{"next",accessorWire(a.steps.at(0),l,depth+1)}};
    if (a.className == "MACompositeAccessor")
        j["kind"] = a.kind ? wire(Value(Value::Class{*a.kind}), l) : wire(Value(), l);
    if (a.className == "MAChainAccessor" || a.className == "MACompositeAccessor") {
        j["steps"] = J::array();
        for (const auto &step : a.steps)
            j["steps"].push_back(accessorWire(step, l, depth + 1));
    } else if (a.className == "MASelectorAccessor") {
        j["readSelector"] = a.name;
        j["writeSelector"] = a.writeName;
    } else if (a.className == "MADictionaryAccessor" || a.className == "MAPropertyAccessor")
        j["key"] = a.name;
    else if (a.className == "MAVariableAccessor")
        j["name"] = a.name;
    return j;
}
Accessor accessorValue(const J &j, Limits l, std::size_t depth = 0) {
    require(depth <= l.depth, "Accessor depth limit");
    Accessor a;
    a.className = string(j.at("class"));
    require(a.className != "MAPluggableAccessor", "Pluggable accessor blocks cannot be imported");
    if (a.className == "MANullAccessor") {
        keys(j, {"class", "uuid"});
        a.uuid = value(j.at("uuid"), l);
    } else if (a.className == "MADelegatorAccessor") {
        keys(j, {"class", "next"});
        a.steps.push_back(accessorValue(j.at("next"), l, depth + 1));
    } else if (a.className == "MAChainAccessor" || a.className == "MACompositeAccessor") {
        if (a.className == "MACompositeAccessor") {
            keys(j, {"class", "steps", "kind"});
            auto kind = value(j.at("kind"), l);
            if (!kind.isNil()) {
                require(kind.data.index() == 10, "Composite kind must be Class or nil");
                a.kind = std::get<Value::Class>(kind.data).name;
            }
        } else
            keys(j, {"class", "steps"});
        require(j.at("steps").is_array(), "Expected accessor steps");
        for (const auto &step : j.at("steps"))
            a.steps.push_back(accessorValue(step, l, depth + 1));
    } else if (a.className == "MASelectorAccessor") {
        keys(j, {"class", "readSelector", "writeSelector"});
        a.name = string(j.at("readSelector"));
        if (!a.name.empty())
            (void)identity(j.at("readSelector"));
        a.writeName = string(j.at("writeSelector"));
        if (!a.writeName.empty())
            (void)identity(j.at("writeSelector"));
    } else if (a.className == "MADictionaryAccessor" || a.className == "MAPropertyAccessor") {
        keys(j, {"class", "key"});
        a.name = identity(j.at("key"));
    } else if (a.className == "MAVariableAccessor") {
        keys(j, {"class", "name"});
        a.name = identity(j.at("name"));
    } else
        keys(j, {"class"});
    a.check(l);
    return a;
}
void supported(const Description &d) {
    static const std::set<std::string> classes{"MAContainer",
                                               "MAPriorityContainer",
                                               "MAStringDescription",
                                               "MASymbolDescription",
                                               "MAMemoDescription",
                                               "MAPasswordDescription",
                                               "MATokenDescription",
                                               "MANumberDescription",
                                               "MABooleanDescription",
                                               "MAToOneRelationDescription",
                                               "MAToManyRelationDescription",
                                               "MAToManyScalarRelationDescription",
                                               "MASingleOptionDescription",
                                               "MAMultipleOptionDescription", "MAClassDescription"};
    require(classes.count(d.className), "Unsupported description class: " + d.className);
    for (const auto &p : d.properties) {
        const auto &k = p.first;
        const auto &v = p.second;
        if (k == "canExpand" || k == "required" || k == "readOnly" || k == "visible" ||
            k == "ordered" || k == "distinct" || k == "definitive" || k == "sorted" ||
            k == "extensible" || k == "shouldCacheDefault")
            require(std::holds_alternative<bool>(v.data), "Expected Boolean property: " + k);
        else if (k == "separators" || k == "trueString" || k == "falseString") {
            require((k == "separators" && d.className == "MATokenDescription") ||
                        (k != "separators" && d.className == "MABooleanDescription"),
                    "Text setting does not apply");
            require(std::holds_alternative<std::string>(v.data), "Expected text setting");
        } else if (k == "thousandsSeparator") {
            require(d.className == "MANumberDescription" && v.data.index() == 6 &&
                        Value(std::get<Value::Character>(v.data).utf8).elements().size() == 1,
                    "Expected thousands separator character");
        } else if (k == "lineCount")
            require(d.className == "MAMemoDescription" && v.data.index() == 2, "Expected memo line count");
        else if (k == "maPrintString" || k == "maPrintStringSeparator")
            require(v.isNil(), "Print settings require local callable slots or explicit nil");
        else if (k == "displayBlockOrSymbol")
            require(v.isNil() || v.data.index() == 8,
                    "Display must be Symbol or nil; use displayBlock for local callbacks");
        else if (k == "priority")
            require(detail::isNumber(v), "Expected numeric priority");
        else if (k == "min" || k == "max") {
            require(d.className == "MANumberDescription", "Range requires number description");
            require(v.isNil() || detail::isNumber(v), "Unsupported numeric bound");
            if (auto n = std::get_if<double>(&v.data))
                (void)detail::floatBits(*n);
        } else if (k == "label" || k == "comment" || k == "group" || k == "undefined")
            require(v.isNil() || std::holds_alternative<std::string>(v.data), "Expected text property: " + k);
        else if (k == "stringReader" || k == "stringWriter" || k == "validator") {
            auto cls=std::get_if<Value::Class>(&v.data);require(cls && cls->name==(k=="stringReader"?"MAStringReader":k=="stringWriter"?"MAStringWriter":"MAValidatorVisitor"), "Unsupported interpreter class override");
        } else if (k == "descriptionBuilder")
            require(v.isNil() || v.data.index() == 10, "Builder must be Class or nil");
        else if (k == "descriptionBuilders") {
            require(d.isContainer() && v.data.index() == 4, "Builders require container Array");
            for (const auto &e : v.elements())
                require(e.data.index() == 10, "Builder entry must be Class");
        } else if (k == "kind")
            require(v.isNil() || (v.data.index() == 10 &&
                                  detail::knownClass(std::get<Value::Class>(v.data).name)),
                    "Description kind must be Class or nil");
        else if (k == "requiredErrorMessage" || k == "kindErrorMessage" ||
                 k == "conflictErrorMessage" || k == "rangeErrorMessage") {
            require(k != "rangeErrorMessage" || ClassRegistry::global().includesBehavior(
                                                    d.className, "MAMagnitudeDescription"),
                    "Range message requires a magnitude description");
            require(v.isNil() || std::holds_alternative<std::string>(v.data),
                    "Error message must be String or nil");
        } else if (k == "initialAnswer")
            require(!d.isContainer(), "Initial answer requires an element description");
        else if (k == "name")
            require(v.isNil() || v.data.index() == 3 || v.data.index() == 8, "Expected description name");
        else if (k != "default" && k != "undefinedValue" && k.rfind("metadata:", 0) != 0)
            throw Error("Unsupported semantic property: " + k);
    }
    require(!d.isContainer() || d.portableConditions.empty(),
            "Portable container conditions require a model message protocol");
    require(d.isContainer() || d.children.empty(), "Scalar cannot contain children");
    if (d.isReference())
        (void)detail::referenceTarget(d);
    else
        require(d.reference.expired(), "Reference slot requires a reference description");
    require(d.isRelation() || d.classes.empty(), "Classes require a relation");
    require(d.isOption() || (d.options.empty() && d.labels.empty()), "Options require an option description");
    for (const auto &k : {"ordered", "distinct", "definitive", "sorted", "extensible"}) {
        if (!d.properties.count(k))
            continue;
        require((std::string(k) == "extensible" && d.isOption()) ||
                    (std::string(k) == "distinct" && d.className == "MAMultipleOptionDescription") ||
                    (std::string(k) == "ordered" && d.isMultiple()) ||
                    (std::string(k) == "definitive" && d.isRelation() && d.isMultiple()) ||
                    (std::string(k) == "sorted" && (d.isOption() || (d.isRelation() && d.isMultiple()))),
                "Property does not apply to this description");
    }
}
Value lookup(const std::map<std::string, Value>& m, const std::string& key) {
    auto it = m.find(key); return it == m.end() ? Value() : it->second;
}
} // namespace
namespace detail { void checkDescription(const Description& d) { supported(d); } }
bool Description::isContainer() const { return className == "MAContainer" || className == "MAPriorityContainer"; }
Value Description::property(const std::string &name, Limits limits) const {
    if (name == "displayBlockOrSymbol" && displayBlock)
        throw Error("Display block is local; inspect displayBlock rather than a Value snapshot");
    if ((name == "maPrintString" && printStringBlock) ||
        (name == "maPrintStringSeparator" && printSeparatorBlock))
        throw Error("Print block is local; inspect its callable slot");
    if (name == "requiredErrorMessage")
        return requiredErrorMessage(limits);
    if (name == "kindErrorMessage")
        return kindErrorMessage(limits);
    if (name == "conflictErrorMessage")
        return conflictErrorMessage(limits);
    if (name == "rangeErrorMessage")
        return rangeErrorMessage(limits);
    auto it = properties.find(name);
    if (it != properties.end()) {
        if (name == "label" || name == "name")
            detail::checkLocalValue(it->second, limits);
        return it->second;
    }
    if (name == "kind") {
        std::string kind = "Object";
        if (className == "MAClassDescription")
            kind = "Class";
        else if (className == "MASymbolDescription")
            kind = "Symbol";
        else if (className == "MANumberDescription")
            kind = "Number";
        else if (className == "MABooleanDescription")
            kind = "Boolean";
        else if (className == "MATokenDescription")
            kind = "Array";
        else if (isMultiple())
            kind = "Collection";
        else if (ClassRegistry::global().includesBehavior(className, "MAStringDescription"))
            kind = "String";
        return Value(Value::Class{kind});
    }
    if (name == "displayBlockOrSymbol")
        return Value(Value::Symbol{"greaseString"});
    if (name == "initialAnswer")
        return initialAnswer(limits);
    if(name=="stringReader")return Value(Value::Class{"MAStringReader"});
    if(name=="stringWriter")return Value(Value::Class{"MAStringWriter"});
    if(name=="validator")return Value(Value::Class{"MAValidatorVisitor"});
    if (name == "separators") return Value(" ,;\r\n\t");
    if (name == "thousandsSeparator") return Value(Value::Character{","});
    if (name == "lineCount") return Value(std::int64_t(3));
    if (name == "trueString") return Value("true");
    if (name == "falseString") return Value("false");
    if (name == "trueStrings" || name == "falseStrings") {
        Value::Array a; for (const auto& word : name == "trueStrings" ? std::vector<std::string>{"true","t","yes","y","1","on"} : std::vector<std::string>{"false","f","no","n","0","off"}) a.emplace_back(word); return Value(a);
    }
    if (name == "visible") return Value(true);
    if (name == "required" || name == "readOnly" || name == "ordered" || name == "distinct" || name == "definitive" || name == "sorted" || name == "extensible") return Value(false);
    if (name == "shouldCacheDefault") return Value(isRelation() && isMultiple());
    if (name == "descriptionBuilders")
        return Value(Value::Array{});
    if (name == "priority") return Value(std::int64_t(0));
    if (name == "undefined") return Value("");
    if (name == "label") {
        auto result = Value(defaultLabel(limits));
        detail::checkLocalValue(result, limits);
        return result;
    }
    if (name == "name")
        return effectiveAccessor(limits).accessorName(limits);
    return {};
}
Value Description::defaultValue() const { return defaultFunction ? defaultFunction() : property(isContainer() ? "undefinedValue" : "default"); }
bool Description::flag(const std::string& name) const {
    auto v = property(name); require(std::holds_alternative<bool>(v.data), "Expected Boolean flag"); return std::get<bool>(v.data);
}
std::int64_t Description::priority() const {
    auto v = property("priority");
    require(std::holds_alternative<std::int64_t>(v.data), "Expected integer priority");
    return std::get<std::int64_t>(v.data);
}
std::vector<std::shared_ptr<Description>> Description::orderedChildren() const {
    auto result = children;
    for (const auto& child : result) require(bool(child), "Null description");
    if (className == "MAPriorityContainer")
        std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            return detail::compareNumbers(a->priorityValue(), b->priorityValue()) < 0;
        });
    return result;
}
Value Description::readUsing(const Description &d) const {
    auto value =
        (d.accessor || d.accessorObject) ? d.effectiveAccessor().read(*this) : property(d.key);
    return detail::undefinedIfNil(d, std::move(value));
}
void Description::writeUsing(const Description& d, Value v) { if(d.accessor || d.accessorObject)d.effectiveAccessor().write(*this,std::move(v));else properties[d.key] = std::move(v); }
Value AdaptiveModel::readUsing(const Description& d) const { return lookup(values, d.id); }
void AdaptiveModel::writeUsing(const Description& d, Value v) { values[d.id] = std::move(v); }
Value DictionaryModel::readUsing(const Description &d) const {
    auto value = d.effectiveAccessor().read(*this, resolver);
    return detail::undefinedIfNil(d, std::move(value));
}
void DictionaryModel::writeUsing(const Description& d, Value v) { d.effectiveAccessor().write(*this, std::move(v), resolver); }
Value NativeModel::readUsing(const Description& d) const {
    auto value = (object_ && d.nativeRead) ? d.nativeRead(object_)
                                           : d.effectiveAccessor().read(*this, resolver);
    return detail::undefinedIfNil(d, std::move(value));
}
void NativeModel::writeUsing(const Description& d, Value v) {
    if (object_ && d.nativeWrite) d.nativeWrite(object_, v);
    else d.effectiveAccessor().write(*this, std::move(v), resolver);
}
namespace {
bool kindOf(const Value& v, const std::string& kind, const Graph* graph) {
    if (auto c = v.conflict())
        return c->kindOf(kind);
    if (auto r = std::get_if<Value::Reference>(&v.data)) {
        require(graph, "Object reference validation requires a graph resolver"); return graph->resolve(*r).kindOf(kind);
    }
    if (v.data.index() == 12)
        return ClassRegistry::global().includesBehavior("Fraction", kind);
    static const char* types[]={"UndefinedObject","Boolean","Integer","String","Array",nullptr,"Character",nullptr,"Symbol","Float","Class"};
    auto type=types[v.data.index()];std::string collection;
    if(auto c=std::get_if<Value::Collection>(&v.data)){collection=c->kind=="ordered"?"OrderedCollection":c->kind=="set"?"Set":"Bag";type=collection.c_str();}
    return type && ClassRegistry::global().includesBehavior(type,kind);
}
struct Budget {
    Limits limits; std::size_t work = 0;
    void tick(std::size_t depth) { require(++work <= limits.visits && depth <= limits.depth, "Validation work/depth limit"); }
};
void checkReferences(const Value& v, const Graph* graph, Budget& budget, std::size_t depth) {
    budget.tick(depth);
    if (auto r = std::get_if<Value::Reference>(&v.data)) { require(graph, "Object references require a graph resolver"); (void)graph->resolve(*r); }
    if (v.data.index() == 4 || v.data.index() == 7) {
        require(v.collectionSize() <= budget.limits.visits - budget.work, "Collection work limit");
        for (const auto& e : v.elements()) checkReferences(e, graph, budget, depth + 1);
    }
}
std::vector<ValidationError> validateField(const Description&, const Value&, const Graph*, Budget&, std::size_t);
std::vector<ValidationError> validateModel(const Description& d, const Model& model, const Graph* graph, Budget& budget, std::size_t depth) {
    budget.tick(depth); supported(d); detail::checkPortableDescription(d,budget.limits,budget.work);
    if (!d.flag("visible") || d.flag("readOnly")) return {};
    if (!d.isContainer()) return validateField(d, model.readUsing(d), graph, budget, depth + 1);
    require(d.conditions.empty(), "Container local conditions are not implemented");
    std::vector<ValidationError> errors;
    for (const auto& c : d.orderedChildren()) {
        std::vector<ValidationError> e;
        if (c->isContainer() && (model.usesStoredContainerValues() || c->nativeRead || ((c->accessor || c->accessorObject) && c->effectiveAccessor().className != "MAIdentityAccessor")))
            e = validateField(*c, model.readUsing(*c), graph, budget, depth + 1);
        else e = validateModel(*c, model, graph, budget, depth + 1);
        errors.insert(errors.end(), e.begin(), e.end());
    }
    return errors;
}
std::vector<ValidationError> validateField(const Description& d, const Value& v, const Graph* graph, Budget& budget, std::size_t depth) {
    budget.tick(depth); supported(d); detail::checkPortableDescription(d,budget.limits,budget.work);
    std::vector<ValidationError> errors;
    if (!d.flag("visible") || d.flag("readOnly")) return errors;
    checkReferences(v, graph, budget, depth);
    auto error = [&](std::string code) {
        auto message = detail::validationMessage(d, code, budget.limits);
        errors.push_back({d.id, std::move(code), std::move(message)});
    };
    if (d.flag("required") && (v.isNil() || (d.isMultiple() && v.isCollection() && v.collectionSize() == 0))) error("MARequiredError");
    if (detail::valueKey(v, budget.limits, budget.work) == detail::valueKey(d.property("undefinedValue"), budget.limits, budget.work)) return errors;
    if (d.isContainer()) {
        require(graph, "Container value validation requires a graph resolver");
        require(std::holds_alternative<Value::Reference>(v.data), "Container value requires object reference");
        auto e = validateModel(d, graph->resolve(v), graph, budget, depth + 1); errors.insert(errors.end(), e.begin(), e.end()); return errors;
    }
    bool kind = true;
    if ((d.isRelation() || d.isOption()) && d.properties.count("kind")) {
        auto expected = d.property("kind");
        require(expected.data.index() == 10, "Description kind is nil");
        if (!kindOf(v, std::get<Value::Class>(expected.data).name, graph)) {
            error("MAKindError");
            return errors;
        }
    }
    if (d.isRelation()) {
        auto check = [&](const Value& e) {
            budget.tick(depth);
            bool accepted = false;
            for (const auto& cls : d.classes) { budget.tick(depth); if (kindOf(e, cls, graph)) accepted = true; }
            if (!accepted) { error("MAKindError"); kind = false; }
        };
        if (d.isMultiple()) {
            if (!v.isCollection()) { error("MAKindError"); kind = false; }
            else {
                require(v.collectionSize() <= budget.limits.visits - budget.work, "Collection work limit");
                for (const auto& e : v.elements()) check(e);
            }
        } else check(v);
    } else if (d.isOption()) {
        auto check = [&](const Value& e) {
            bool member = false;
            if (d.options.isDynamic()) { budget.tick(depth); member = d.options.provider()->includes(e, budget.limits); }
            else for (const auto& option : d.options.values(budget.limits)) { budget.tick(depth); if (detail::valueKey(e, budget.limits, budget.work) == detail::valueKey(option, budget.limits, budget.work)) { member = true; break; } }
            if (member) return;
            if (!d.flag("extensible")) { error("MAKindError"); kind = false; return; }
            auto nested = validateField(*d.referenceDescription(), e, graph, budget, depth + 1);
            if (!nested.empty()) kind = false;
            errors.insert(errors.end(), nested.begin(), nested.end());
        };
        if (d.isMultiple()) {
            if (!v.isCollection()) { error("MAKindError"); kind = false; }
            else {
                require(v.collectionSize() <= budget.limits.visits - budget.work, "Collection work limit");
                for (const auto& e : v.elements()) check(e);
            }
        } else check(v);
    } else {
        auto expected = d.property("kind");
        require(expected.data.index() == 10, "Description kind is nil");
        kind = kindOf(v, std::get<Value::Class>(expected.data).name, graph);
        if (!kind) error("MAKindError");
    }
    if (!kind) return errors;
    if (d.className == "MANumberDescription") {
        auto min = d.property("min"), max = d.property("max");
        if ((!min.isNil() && detail::compareNumbers(v, min) < 0) || (!max.isNil() && detail::compareNumbers(v, max) > 0)) error("MARangeError");
    }
    for (const auto& c : d.portableConditions) {
        if (!detail::evaluatePredicate(c.predicate, v, budget.limits, budget.work)) errors.push_back({d.id, "MAConditionError", c.label});
    }
    for (const auto& c : d.conditions) {
        budget.tick(depth); require(bool(c.test), "Missing local condition function");
        if (!c.test(v)) errors.push_back({d.id, "MAConditionError", c.label});
    }
    return errors;
}
}
std::vector<ValidationError> validateValue(const Description& d, const Value& v, const Graph* graph, Limits l) {
    Budget budget{l}; return validateField(d, v, graph, budget, 0);
}
void visit(const Description& root, const std::function<void(const Description&)>& f, Limits l) {
    std::set<const Description*> active, composed, seen;
    std::size_t work = 0;
    std::function<void(const Description&, std::size_t)> composition = [&](const auto& d, auto depth) {
        require(++work <= l.visits && depth <= l.depth, "Description traversal limit");
        require(!active.count(&d), "Cyclic composition is unsupported");
        if (composed.count(&d)) return;
        active.insert(&d);
        for (const auto& child : d.orderedChildren()) composition(*child, depth + 1);
        active.erase(&d); composed.insert(&d);
    };
    std::function<void(const Description&, std::size_t)> walk = [&](const auto& d, auto depth) {
        require(++work <= l.visits && depth <= l.depth, "Description traversal limit");
        if (!seen.insert(&d).second) return;
        require(seen.size() <= l.nodes, "Description node limit");
        composition(d, 0); f(d);
        for (const auto& c : d.orderedChildren()) walk(*c, depth + 1);
        if (d.isReference())
            walk(*detail::referenceTarget(d), depth + 1);
    };
    walk(root, 0);
}
std::vector<ValidationError> validate(const Description& root, const Model& model, Limits l, const Graph* graph) {
    std::size_t work=0;
    visit(root, [&](const auto& d) { supported(d); detail::checkPortableDescription(d,l,work); }, l);
    Budget budget{l,work}; return validateModel(root, model, graph, budget, 0);
}
Predicate decodePredicate(const std::string& s,Limits l) { try { return predicateValue(parse(s,l),l); } catch(const J::exception& e){throw Error(e.what());} }
std::string encodePredicate(const Predicate& p,Limits l) { p.check(l);auto s=predicateWire(p,l).dump();(void)decodePredicate(s,l);return s; }
Accessor decodeAccessor(const std::string& s,Limits l) { try { return accessorValue(parse(s,l),l); } catch(const J::exception& e){throw Error(e.what());} }
std::string encodeAccessor(const Accessor& a,Limits l) { a.check(l);auto s=accessorWire(a,l).dump();(void)decodeAccessor(s,l);return s; }
Value decodeValue(const std::string& input, Limits l) {
    try { return value(parse(input, l), l); } catch (const J::exception& e) { throw Error(e.what()); }
}
std::string encodeValue(const Value& v, Limits l) {
    try { auto s = wire(v, l).dump(); (void)value(parse(s, l), l); return s; } catch (const J::exception& e) { throw Error(e.what()); }
}
Graph Graph::decode(const std::string& input, Limits l) {
    try {
        auto j = parse(input, l);
        keys(j, {"format", "root", "descriptions", "objects"});
        require(j.at("format") == "magritte.graph/1" || j.at("format") == "magritte.graph/2" || j.at("format") == "magritte.graph/3" || j.at("format") == "magritte.graph/4", "Unsupported format");
        if(j.at("format")!="magritte.graph/4") {
            std::function<void(const J &)> legacy = [&](const J &node) {
                if (node.is_object() &&
                    ((node.contains("type") && node.at("type").is_string() &&
                      (node.at("type") == "class" || node.at("type") == "fraction")) ||
                     (node.contains("class") && node.at("class").is_string() &&
                      (node.at("class") == "MADelegatorAccessor" ||
                       node.at("class") == "MACompositeAccessor"))))
                    throw Error("Class values and delegators require graph/4");
                if (node.is_structured())
                    for (const auto &child : node)
                        legacy(child);
            };
            legacy(j);
        }
        Graph g; g.format = string(j.at("format")); g.root = identity(j.at("root"));
        require(j.at("descriptions").is_array() && j.at("descriptions").size() <= l.nodes, "Description node limit");
        for (const auto& n : j.at("descriptions")) {
            auto d = std::make_shared<Description>();
            d->id = identity(n.at("id")); d->className = string(n.at("class"));
            auto shape = n; if (g.format >= "magritte.graph/3") shape.erase("conditions");
            if (n.contains("definingContext")) {
                require(g.format == "magritte.graph/4", "Defining context requires graph/4");
                shape.erase("definingContext");
                const auto &context = n.at("definingContext");
                keys(context, {"modelClass", "methodSelector"});
                auto cls = value(context.at("modelClass"), l),
                     selector = value(context.at("methodSelector"), l);
                require(cls.isNil() || cls.data.index() == 10,
                        "Context modelClass must be Class or nil");
                require(selector.isNil() || selector.data.index() == 8,
                        "Context selector must be Symbol or nil");
                d->definingContext = std::make_shared<DescriptionContext>();
                if (!cls.isNil())
                    d->definingContext->modelClass = std::get<Value::Class>(cls.data).name;
                if (!selector.isNil())
                    d->definingContext->methodSelector =
                        std::get<Value::Symbol>(selector.data).utf8;
            }
            if (d->isContainer()) keys(shape, {"id", "class", "properties", "children"});
            else if (d->isRelation()) keys(shape, {"id", "class", "properties", "accessor", "reference", "classes"});
            else if (d->isOption()) keys(shape, {"id", "class", "properties", "accessor", "reference", "options", "labels"});
            else if (d->className == "MATokenDescription") keys(shape, {"id", "class", "properties", "accessor", "reference"});
            else keys(shape, {"id", "class", "properties", "accessor"});
            require(g.format == "magritte.graph/4" || d->className != "MAClassDescription", "Class descriptions require graph/4");
            require(g.format != "magritte.graph/1" || !d->isReference(), "Reference descriptions require graph/2");
            require(n.at("properties").is_object(), "Expected properties dictionary");
            for (auto it = n.at("properties").begin(); it != n.at("properties").end(); ++it) {
                (void)identity(J(it.key()));
                require(it.key() != "maPrintString" && it.key() != "maPrintStringSeparator",
                        "Print settings are local callable state");
                d->properties.emplace(it.key(), value(it.value(), l));
            }
            if (!d->isContainer()) {
                auto a = accessorValue(n.at("accessor"),l);
                require(g.format >= "magritte.graph/3" || a.className == "MADictionaryAccessor", "Accessor requires graph/3");
                if (a.className == "MADictionaryAccessor") d->key = a.name;
                else d->accessor = a;
            }
            if (g.format >= "magritte.graph/3") {
                require(n.at("conditions").is_array(),"Expected conditions list");
                for(const auto& c:n.at("conditions")){ keys(c,{"label","predicate"});d->portableConditions.push_back({string(c.at("label")),predicateValue(c.at("predicate"),l)}); }
            } else {
                require(d->className != "MATokenDescription" && d->className != "MASymbolDescription" && d->className != "MAMemoDescription" && d->className != "MAPasswordDescription", "Description requires graph/3");
                for (const auto &prop : d->properties)
                    require(
                        prop.first != "separators" && prop.first != "thousandsSeparator" &&
                            prop.first != "lineCount" && prop.first != "trueString" &&
                            prop.first != "falseString" && prop.first != "trueStrings" &&
                            prop.first != "falseStrings" && prop.first != "displayBlockOrSymbol" &&
                            prop.first != "initialAnswer" && prop.first != "requiredErrorMessage" &&
                            prop.first != "kindErrorMessage" && prop.first != "rangeErrorMessage" &&
                            prop.first != "conflictErrorMessage",
                        "Property requires graph/3");
            }
            require(g.descriptions.emplace(d->id, d).second, "Duplicate description identity");
        }
        // Reject owning composition cycles before creating shared_ptr child edges.
        std::map<std::string, std::vector<std::string>> composition;
        for (const auto& n : j.at("descriptions")) {
            auto id = string(n.at("id"));
            if (!g.descriptions.at(id)->isContainer()) continue;
            require(n.at("children").is_array(), "Expected children array");
            for (const auto& c : n.at("children")) {
                auto child = identity(c); require(g.descriptions.count(child), "Dangling description identity");
                composition[id].push_back(child);
            }
        }
        std::set<std::string> activeComposition, completeComposition; std::size_t compositionWork = 0;
        std::function<void(const std::string&, std::size_t)> checkComposition = [&](const auto& id, auto depth) {
            require(++compositionWork <= l.visits && depth <= l.depth, "Composition work/depth limit");
            require(!activeComposition.count(id), "Cyclic composition is unsupported");
            if (completeComposition.count(id)) return;
            activeComposition.insert(id);
            for (const auto& child : composition[id]) checkComposition(child, depth + 1);
            activeComposition.erase(id); completeComposition.insert(id);
        };
        for (const auto& p : g.descriptions) if (!completeComposition.count(p.first)) checkComposition(p.first, 0);
        std::size_t portableWork=0;
        for (const auto& n : j.at("descriptions")) {
            auto d = g.descriptions.at(string(n.at("id")));
            if (d->isReference()) {
                auto ref = identity(n.at("reference")); require(g.descriptions.count(ref), "Dangling reference description");
                d->reference = g.descriptions.at(ref);
            }
            if (d->isRelation()) {
                require(n.at("classes").is_array(), "Expected classes array");

                std::set<std::string> unique;
                for (const auto& c : n.at("classes")) { auto cls = identity(c); require((g.format >= "magritte.graph/3" || (cls != "Symbol" && cls != "Float" && (cls.rfind("MA", 0) != 0 || cls == "MABasicObject" || cls == "MAAdaptiveModel"))) && detail::knownClass(cls) && unique.insert(cls).second, "Unsupported or duplicate class constraint"); d->classes.push_back(cls); }
            }
            if (d->isOption()) {
                require(n.at("options").is_array() && n.at("labels").is_array(), "Expected options and labels arrays");
                for (const auto& option : n.at("options")) d->options.push_back(value(option, l));
                std::set<std::string> labelKeys; std::size_t labelWork = 0;
                for (const auto& label : n.at("labels")) {
                    keys(label, {"value", "label"}); auto v = value(label.at("value"), l);
                    require(detail::portableLabelKey(v), "Identity-sensitive option label key is not portable");
                    require(labelKeys.insert(detail::valueKey(v, l, labelWork)).second, "Duplicate label key");
                    d->labels.emplace_back(std::move(v), string(label.at("label")));
                }
            }
            supported(*d); detail::checkPortableDescription(*d,l,portableWork);
            if (g.format == "magritte.graph/1") {
                static const std::set<std::string> oldProperties{"required", "readOnly", "visible", "priority", "min", "max", "label", "comment", "group", "undefined", "default", "undefinedValue"};
                for (const auto& p : d->properties) require(oldProperties.count(p.first) || p.first.rfind("metadata:", 0) == 0, "Property requires graph/2");
            }
            if (!d->isContainer()) continue;
            require(n.at("children").is_array(), "Expected children array");
            std::set<std::string> unique;
            for (const auto& c : n.at("children")) {
                auto id = identity(c);
                require(unique.insert(id).second, "Duplicate container child");
                require(g.descriptions.count(id), "Dangling description identity");
                auto child = g.descriptions.at(id);
                require(g.format != "magritte.graph/1" || !child->isContainer(), "Nested containers require graph/2");
                d->children.push_back(child);
            }
        }
        require(g.descriptions.count(g.root) && g.descriptions.at(g.root)->isContainer(), "Root must identify a container");
        std::set<const Description*> checked;
        std::size_t graphWork = 0;
        for (const auto& p : g.descriptions) if (!checked.count(p.second.get())) visit(*p.second, [&](const auto& d) { require(++graphWork <= l.visits, "Graph traversal work limit"); checked.insert(&d); }, l);
        require(j.at("objects").is_array() && j.at("objects").size() <= l.nodes, "Object node limit");
        for (const auto& o : j.at("objects")) {
            keys(o, {"id", "description", "values"});
            AdaptiveModel m; m.id = identity(o.at("id"));
            auto id = identity(o.at("description")); require(g.descriptions.count(id), "Dangling object description");
            m.description = g.descriptions.at(id); require(m.description->isContainer(), "Object description must be a container");
            require(o.at("values").is_object(), "Expected object value dictionary");
            std::set<std::string> fields;
            for (const auto& d : m.description->children) fields.insert(d->id);
            for (auto it = o.at("values").begin(); it != o.at("values").end(); ++it) {
                require(fields.count(it.key()), "Value is not described by the object container");
                m.values.emplace(it.key(), value(it.value(), l));
            }
            require(g.objects.emplace(m.id, std::move(m)).second, "Duplicate object identity");
        }
        std::function<void(const Value&, std::size_t)> references = [&](const auto& v, auto depth) {
            require(depth <= l.depth && ++graphWork <= l.visits, "Value reference traversal limit");
            if (auto r = std::get_if<Value::Reference>(&v.data)) { require(g.format != "magritte.graph/1", "Object references require graph/2"); (void)g.resolve(*r); }
            if(v.data.index()==10) require(g.format=="magritte.graph/4", "Class values require graph/4");
            if (v.data.index() == 8 || v.data.index() == 9) require(g.format >= "magritte.graph/3", "Symbol/Float requires graph/3");
            if (v.data.index() == 6 || v.data.index() == 7) require(g.format != "magritte.graph/1", "Typed collections/characters require graph/2");
            if (v.data.index() == 4 || v.data.index() == 7) for (const auto& e : v.elements()) references(e, depth + 1);
        };
        for (const auto& p : g.descriptions) {
            for (const auto& property : p.second->properties) references(property.second, 0);
            for (const auto& option : p.second->options.values(l)) references(option, 0);
            for (const auto& label : p.second->labels) references(label.first, 0);
        }
        for (const auto& o : g.objects) for (const auto& v : o.second.values) references(v.second, 0);
        return g;
    } catch (const J::exception& e) { throw Error(e.what()); }
}
std::string Graph::encode(Limits l) const {
    try {
        require(nativeObjects.empty(), "Local native object bindings are not portable");
        J j{{"format", format}, {"root", root}, {"descriptions", J::array()}, {"objects", J::array()}};
        require(descriptions.size() <= l.nodes && objects.size() <= l.nodes, "Graph node limit");
        std::size_t portableWork=0;
        for (const auto& p : descriptions) {
            require(bool(p.second) && p.first == p.second->id, "Inconsistent description identity");
            const auto& d = *p.second; supported(d); detail::checkPortableDescription(d,l,portableWork);
            require(!d.options.isDynamic(), "Local dynamic option providers are not portable; replace with explicit static options before export");
            require(!d.accessorObject, "Live accessor identity is local; materialize an explicit snapshot before export");
            require(d.conditions.empty() && !d.nativeRead && !d.nativeWrite && !d.defaultFunction &&
                        !d.displayBlock && !d.printStringBlock && !d.printSeparatorBlock &&
                        !d.properties.count("maPrintString") &&
                        !d.properties.count("maPrintStringSeparator"),
                    "Local executable behavior is not portable");
            J n{{"id", d.id}, {"class", d.className}, {"properties", J::object()}};
            if (d.definingContext) {
                require(format == "magritte.graph/4", "Defining context requires graph/4");
                const auto &context = *d.definingContext;
                n["definingContext"] =
                    J{{"modelClass",
                       wire(context.modelClass ? Value(Value::Class{*context.modelClass}) : Value(),
                            l)},
                      {"methodSelector",
                       wire(context.methodSelector ? Value(Value::Symbol{*context.methodSelector})
                                                   : Value(),
                            l)}};
            }
            for (const auto& prop : d.properties) n["properties"][prop.first] = wire(prop.second, l);
            if (format >= "magritte.graph/3") {
                n["conditions"] = J::array();
                for(const auto& c:d.portableConditions)n["conditions"].push_back(J{{"label",c.label},{"predicate",predicateWire(c.predicate,l)}});
            } else require(d.portableConditions.empty(),"Conditions require graph/3");
            if (d.isContainer()) {
                require(!d.accessor,"Container accessors are implicit identity");
                n["children"] = J::array();
                for (const auto& c : d.children) {
                    require(c && descriptions.count(c->id) && descriptions.at(c->id) == c, "Unregistered child identity");
                    n["children"].push_back(c->id);
                }
            } else n["accessor"] = accessorWire(d.effectiveAccessor(),l);
            if (d.isReference()) {
                auto r = detail::referenceTarget(d);
                require(descriptions.count(r->id) && descriptions.at(r->id) == r, "Unregistered reference description");
                n["reference"] = r->id;
            }
            if (d.isRelation()) n["classes"] = d.classes;
            if (d.isOption()) {
                n["options"] = J::array(); n["labels"] = J::array();
                for (const auto& option : d.options.values(l)) n["options"].push_back(wire(option, l));
                for (const auto& label : d.labels) n["labels"].push_back(J{{"value", wire(label.first, l)}, {"label", label.second}});
            }
            j["descriptions"].push_back(n);
        }
        for (const auto& p : objects) {
            const auto& m = p.second;
            require(p.first == m.id && m.description && descriptions.count(m.description->id) && descriptions.at(m.description->id) == m.description, "Unregistered object description");
            J o{{"id", m.id}, {"description", m.description->id}, {"values", J::object()}};
            for (const auto& v : m.values) o["values"][v.first] = wire(v.second, l);
            j["objects"].push_back(o);
        }
        auto s = j.dump(2); (void)Graph::decode(s, l); return s;
    } catch (const J::exception& e) { throw Error(e.what()); }
}
std::shared_ptr<Description> descriptionModel() {
    auto container = std::make_shared<Description>();
    container->id = "MADescription.bootstrap"; container->className = "MAContainer";
    for (const auto& key : {"label", "comment", "group", "required", "readOnly", "visible", "priority"}) {
        auto d = std::make_shared<Description>(); d->id = std::string("MADescription.") + key; d->key = key;
        d->className = (d->key == "required" || d->key == "readOnly" || d->key == "visible") ? "MABooleanDescription" : (d->key == "priority" ? "MANumberDescription" : "MAStringDescription");
        d->properties["label"] = Value(key); container->children.push_back(d);
    }
    return container;
}
} // namespace magritte
