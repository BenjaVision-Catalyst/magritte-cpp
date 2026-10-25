#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include "detail.hpp"
#include <algorithm>
#include <charconv>
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
    if (type == "string") return Value(string(v));
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
        case 0: return J{{"type", "nil"}};
        case 1: return J{{"type", "boolean"}, {"value", std::get<bool>(v.data)}};
        case 2: return J{{"type", "integer"}, {"value", std::to_string(std::get<std::int64_t>(v.data))}};
        case 3: return J{{"type", "string"}, {"value", std::get<std::string>(v.data)}};
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
void supported(const Description& d) {
    static const std::set<std::string> classes{"MAContainer", "MAPriorityContainer", "MAStringDescription", "MANumberDescription", "MABooleanDescription", "MAToOneRelationDescription", "MAToManyRelationDescription", "MAToManyScalarRelationDescription", "MASingleOptionDescription", "MAMultipleOptionDescription"};
    require(classes.count(d.className), "Unsupported description class: " + d.className);
    for (const auto& p : d.properties) {
        const auto& k = p.first;
        const auto& v = p.second;
        if (k == "required" || k == "readOnly" || k == "visible" || k == "ordered" || k == "distinct" || k == "definitive" || k == "sorted" || k == "extensible" || k == "shouldCacheDefault")
            require(std::holds_alternative<bool>(v.data), "Expected Boolean property: " + k);
        else if (k == "priority") require(std::holds_alternative<std::int64_t>(v.data), "Expected integer priority");
        else if (k == "min" || k == "max") {
            require(d.className == "MANumberDescription", "Range requires number description");
            require(v.isNil() || std::holds_alternative<std::int64_t>(v.data), "Unsupported numeric bound");
        } else if (k == "label" || k == "comment" || k == "group" || k == "undefined")
            require(v.isNil() || std::holds_alternative<std::string>(v.data), "Expected text property: " + k);
        else if (k != "default" && k != "undefinedValue" && k.rfind("metadata:", 0) != 0)
            throw Error("Unsupported semantic property: " + k);
    }
    require(d.isContainer() || d.children.empty(), "Scalar cannot contain children");
    if (d.isReference()) (void)d.referenceDescription();
    else require(d.reference.expired(), "Reference slot requires a reference description");
    require(d.isRelation() || d.classes.empty(), "Classes require a relation");
    require(d.isOption() || (d.options.empty() && d.labels.empty()), "Options require an option description");
    for (const auto& k : {"ordered", "distinct", "definitive", "sorted", "extensible"}) {
        if (!d.properties.count(k)) continue;
        require((std::string(k) == "extensible" && d.isOption()) ||
                (std::string(k) == "distinct" && d.className == "MAMultipleOptionDescription") ||
                (std::string(k) == "ordered" && d.isMultiple()) ||
                (std::string(k) == "definitive" && d.isRelation() && d.isMultiple()) ||
                (std::string(k) == "sorted" && (d.isOption() || (d.isRelation() && d.isMultiple()))), "Property does not apply to this description");
    }
}
Value lookup(const std::map<std::string, Value>& m, const std::string& key) {
    auto it = m.find(key); return it == m.end() ? Value() : it->second;
}
} // namespace
bool Description::isContainer() const { return className == "MAContainer" || className == "MAPriorityContainer"; }
Value Description::property(const std::string& name) const {
    auto it = properties.find(name);
    if (it != properties.end()) return it->second;
    if (name == "visible") return Value(true);
    if (name == "required" || name == "readOnly" || name == "ordered" || name == "distinct" || name == "definitive" || name == "sorted" || name == "extensible") return Value(false);
    if (name == "shouldCacheDefault") return Value(isRelation() && isMultiple());
    if (name == "priority") return Value(std::int64_t(0));
    if (name == "undefined") return Value("");
    if (name == "label") return Value(key); // Explicit difference: no Smalltalk camel-case inflection yet.
    return {};
}
Value Description::defaultValue() const { return defaultFunction ? defaultFunction() : property("default"); }
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
    if (className == "MAPriorityContainer") std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a->priority() < b->priority(); });
    return result;
}
Value Description::readUsing(const Description& d) const { return property(d.key); }
void Description::writeUsing(const Description& d, Value v) { properties[d.key] = std::move(v); }
Value AdaptiveModel::readUsing(const Description& d) const { return lookup(values, d.id); }
void AdaptiveModel::writeUsing(const Description& d, Value v) { values[d.id] = std::move(v); }
Value DictionaryModel::readUsing(const Description& d) const { require(!d.isContainer(), "Identity container access requires the model validator"); return lookup(values, d.key); }
void DictionaryModel::writeUsing(const Description& d, Value v) { require(!d.isContainer(), "Identity container write is unsupported"); values[d.key] = std::move(v); }
Value NativeModel::readUsing(const Description& d) const {
    require(object_ && bool(d.nativeRead), "Native read accessor unavailable"); return d.nativeRead(object_);
}
void NativeModel::writeUsing(const Description& d, Value v) {
    require(object_ && bool(d.nativeWrite), "Native write accessor unavailable"); d.nativeWrite(object_, v);
}
namespace {
bool kindOf(const Value& v, const std::string& kind, const Graph* graph) {
    if (auto r = std::get_if<Value::Reference>(&v.data)) {
        require(graph, "Object reference validation requires a graph resolver"); return graph->resolve(*r).kindOf(kind);
    }
    if (kind == "Object") return true;
    if (kind == "UndefinedObject") return v.isNil();
    if (kind == "String") return v.data.index() == 3;
    if (kind == "Character") return v.data.index() == 6;
    if (kind == "Boolean") return v.data.index() == 1;
    if (kind == "Number" || kind == "Integer") return v.data.index() == 2;
    if (kind == "Collection") return v.isCollection();
    if (kind == "SequenceableCollection") return v.data.index() == 3 || v.data.index() == 4 || (v.data.index() == 7 && std::get<Value::Collection>(v.data).kind == "ordered");
    if (kind == "Array") return v.data.index() == 4;
    if (auto c = std::get_if<Value::Collection>(&v.data)) return (kind == "Set" && c->kind == "set") || (kind == "Bag" && c->kind == "bag") || (kind == "OrderedCollection" && c->kind == "ordered");
    return false;
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
    budget.tick(depth); supported(d);
    if (!d.flag("visible") || d.flag("readOnly")) return {};
    if (!d.isContainer()) return validateField(d, model.readUsing(d), graph, budget, depth + 1);
    require(d.conditions.empty(), "Container local conditions are not implemented");
    std::vector<ValidationError> errors;
    for (const auto& c : d.orderedChildren()) {
        std::vector<ValidationError> e;
        if (c->isContainer() && (dynamic_cast<const AdaptiveModel*>(&model) || c->nativeRead))
            e = validateField(*c, model.readUsing(*c), graph, budget, depth + 1);
        else e = validateModel(*c, model, graph, budget, depth + 1);
        errors.insert(errors.end(), e.begin(), e.end());
    }
    return errors;
}
std::vector<ValidationError> validateField(const Description& d, const Value& v, const Graph* graph, Budget& budget, std::size_t depth) {
    budget.tick(depth); supported(d);
    std::vector<ValidationError> errors;
    if (!d.flag("visible") || d.flag("readOnly")) return errors;
    checkReferences(v, graph, budget, depth);
    auto error = [&](std::string code) { errors.push_back({d.id, std::move(code), "Value does not satisfy description"}); };
    if (d.flag("required") && (v.isNil() || (d.isMultiple() && v.isCollection() && v.collectionSize() == 0))) error("MARequiredError");
    if (detail::valueKey(v, budget.limits, budget.work) == detail::valueKey(d.property("undefinedValue"), budget.limits, budget.work)) return errors;
    if (d.isContainer()) {
        require(graph, "Container value validation requires a graph resolver");
        require(std::holds_alternative<Value::Reference>(v.data), "Container value requires object reference");
        auto e = validateModel(d, graph->resolve(v), graph, budget, depth + 1); errors.insert(errors.end(), e.begin(), e.end()); return errors;
    }
    bool kind = true;
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
            for (const auto& option : d.options) { budget.tick(depth); if (detail::valueKey(e, budget.limits, budget.work) == detail::valueKey(option, budget.limits, budget.work)) { member = true; break; } }
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
        kind = kindOf(v, d.className == "MAStringDescription" ? "String" : d.className == "MANumberDescription" ? "Number" : "Boolean", graph);
        if (!kind) error("MAKindError");
    }
    if (!kind) return errors;
    if (d.className == "MANumberDescription") {
        auto n = std::get<std::int64_t>(v.data); auto min = d.property("min"), max = d.property("max");
        if ((!min.isNil() && n < std::get<std::int64_t>(min.data)) || (!max.isNil() && n > std::get<std::int64_t>(max.data))) error("MARangeError");
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
        if (d.isReference()) walk(*d.referenceDescription(), depth + 1);
    };
    walk(root, 0);
}
std::vector<ValidationError> validate(const Description& root, const Model& model, Limits l, const Graph* graph) {
    visit(root, [](const auto& d) { supported(d); }, l);
    Budget budget{l}; return validateModel(root, model, graph, budget, 0);
}
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
        require(j.at("format") == "magritte.graph/1" || j.at("format") == "magritte.graph/2", "Unsupported format");
        Graph g; g.format = string(j.at("format")); g.root = identity(j.at("root"));
        require(j.at("descriptions").is_array() && j.at("descriptions").size() <= l.nodes, "Description node limit");
        for (const auto& n : j.at("descriptions")) {
            auto d = std::make_shared<Description>();
            d->id = identity(n.at("id")); d->className = string(n.at("class"));
            if (d->isContainer()) keys(n, {"id", "class", "properties", "children"});
            else if (d->isRelation()) keys(n, {"id", "class", "properties", "accessor", "reference", "classes"});
            else if (d->isOption()) keys(n, {"id", "class", "properties", "accessor", "reference", "options", "labels"});
            else keys(n, {"id", "class", "properties", "accessor"});
            require(g.format == "magritte.graph/2" || !d->isReference(), "Reference descriptions require graph/2");
            require(n.at("properties").is_object(), "Expected properties dictionary");
            for (auto it = n.at("properties").begin(); it != n.at("properties").end(); ++it) { (void)identity(J(it.key())); d->properties.emplace(it.key(), value(it.value(), l)); }
            if (!d->isContainer()) {
                const auto& a = n.at("accessor"); keys(a, {"class", "key"});
                require(a.at("class") == "MADictionaryAccessor", "Unsupported accessor strategy");
                d->key = identity(a.at("key"));
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
        for (const auto& n : j.at("descriptions")) {
            auto d = g.descriptions.at(string(n.at("id")));
            if (d->isReference()) {
                auto ref = identity(n.at("reference")); require(g.descriptions.count(ref), "Dangling reference description");
                d->reference = g.descriptions.at(ref);
            }
            if (d->isRelation()) {
                require(n.at("classes").is_array(), "Expected classes array");
                static const std::set<std::string> kinds{"Object", "MABasicObject", "MAAdaptiveModel", "UndefinedObject", "String", "Boolean", "Number", "Integer", "Character", "Collection", "SequenceableCollection", "Array", "OrderedCollection", "Set", "Bag"};
                std::set<std::string> unique;
                for (const auto& c : n.at("classes")) { auto cls = identity(c); require(kinds.count(cls) && unique.insert(cls).second, "Unsupported or duplicate class constraint"); d->classes.push_back(cls); }
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
            supported(*d);
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
                require(g.format == "magritte.graph/2" || !child->isContainer(), "Nested containers require graph/2");
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
            if (auto r = std::get_if<Value::Reference>(&v.data)) { require(g.format == "magritte.graph/2", "Object references require graph/2"); (void)g.resolve(*r); }
            if (v.data.index() == 6 || v.data.index() == 7) require(g.format == "magritte.graph/2", "Typed collections/characters require graph/2");
            if (v.data.index() == 4 || v.data.index() == 7) for (const auto& e : v.elements()) references(e, depth + 1);
        };
        for (const auto& p : g.descriptions) {
            for (const auto& property : p.second->properties) references(property.second, 0);
            for (const auto& option : p.second->options) references(option, 0);
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
        for (const auto& p : descriptions) {
            require(bool(p.second) && p.first == p.second->id, "Inconsistent description identity");
            const auto& d = *p.second; supported(d);
            require(d.conditions.empty() && !d.nativeRead && !d.nativeWrite && !d.defaultFunction, "Local executable behavior is not portable");
            J n{{"id", d.id}, {"class", d.className}, {"properties", J::object()}};
            for (const auto& prop : d.properties) n["properties"][prop.first] = wire(prop.second, l);
            if (d.isContainer()) {
                n["children"] = J::array();
                for (const auto& c : d.children) {
                    require(c && descriptions.count(c->id) && descriptions.at(c->id) == c, "Unregistered child identity");
                    n["children"].push_back(c->id);
                }
            } else n["accessor"] = J{{"class", "MADictionaryAccessor"}, {"key", d.key}};
            if (d.isReference()) {
                auto r = d.referenceDescription();
                require(descriptions.count(r->id) && descriptions.at(r->id) == r, "Unregistered reference description");
                n["reference"] = r->id;
            }
            if (d.isRelation()) n["classes"] = d.classes;
            if (d.isOption()) {
                n["options"] = J::array(); n["labels"] = J::array();
                for (const auto& option : d.options) n["options"].push_back(wire(option, l));
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
