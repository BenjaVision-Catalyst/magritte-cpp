#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
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
    if (type == "boolean") { require(v.is_boolean(), "Expected Boolean"); return Value(v.get<bool>()); }
    if (type == "integer") {
        auto s = string(v);
        std::int64_t i{};
        auto r = std::from_chars(s.data(), s.data() + s.size(), i);
        require(r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::to_string(i) == s,
                "Unsupported or noncanonical integer");
        return Value(i);
    }
    if (type == "array") {
        require(v.is_array() && v.size() <= l.items, "Expected bounded array");
        Value::Array a;
        for (const auto& e : v) a.push_back(value(e, l, depth + 1));
        return Value(std::move(a));
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
        default: {
            auto a = J::array();
            const auto& values = std::get<Value::Array>(v.data);
            require(values.size() <= l.items, "Value item limit");
            for (const auto& e : values) a.push_back(wire(e, l, depth + 1));
            return J{{"type", "array"}, {"value", a}};
        }
    }
}
void supported(const Description& d) {
    static const std::set<std::string> classes{"MAContainer", "MAPriorityContainer", "MAStringDescription", "MANumberDescription", "MABooleanDescription"};
    require(classes.count(d.className), "Unsupported description class: " + d.className);
    for (const auto& p : d.properties) {
        const auto& k = p.first;
        const auto& v = p.second;
        if (k == "required" || k == "readOnly" || k == "visible")
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
    if (name == "required" || name == "readOnly") return Value(false);
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
Value DictionaryModel::readUsing(const Description& d) const { return lookup(values, d.key); }
void DictionaryModel::writeUsing(const Description& d, Value v) { values[d.key] = std::move(v); }
Value NativeModel::readUsing(const Description& d) const {
    require(object_ && bool(d.nativeRead), "Native read accessor unavailable"); return d.nativeRead(object_);
}
void NativeModel::writeUsing(const Description& d, Value v) {
    require(object_ && bool(d.nativeWrite), "Native write accessor unavailable"); d.nativeWrite(object_, v);
}
std::vector<ValidationError> validateValue(const Description& d, const Value& v) {
    supported(d);
    std::vector<ValidationError> errors;
    if (!d.flag("visible") || d.flag("readOnly")) return errors;
    auto error = [&](std::string code, std::string message) { errors.push_back({d.id, std::move(code), std::move(message)}); };
    if (d.flag("required") && v.isNil()) error("MARequiredError", "A value is required");
    if (v == d.property("undefinedValue")) return errors;
    require(!d.isContainer(), "Use model validation for containers");
    bool kind = (d.className == "MAStringDescription" && std::holds_alternative<std::string>(v.data)) ||
                (d.className == "MANumberDescription" && std::holds_alternative<std::int64_t>(v.data)) ||
                (d.className == "MABooleanDescription" && std::holds_alternative<bool>(v.data));
    if (!kind) { error("MAKindError", "Value has the wrong kind"); return errors; }
    if (d.className == "MANumberDescription") {
        auto n = std::get<std::int64_t>(v.data);
        auto min = d.property("min"), max = d.property("max");
        if ((!min.isNil() && n < std::get<std::int64_t>(min.data)) || (!max.isNil() && n > std::get<std::int64_t>(max.data)))
            error("MARangeError", "Value is outside the inclusive range");
    }
    for (const auto& c : d.conditions) {
        require(bool(c.test), "Missing local condition function");
        if (!c.test(v)) error("MAConditionError", c.label);
    }
    return errors;
}
void visit(const Description& root, const std::function<void(const Description&)>& f, Limits l) {
    std::set<const Description*> active, seen;
    std::size_t work = 0;
    std::function<void(const Description&, std::size_t)> walk = [&](const auto& d, auto depth) {
        require(++work <= l.visits && depth <= l.depth, "Description traversal limit");
        require(!active.count(&d), "Cyclic composition is unsupported");
        if (!seen.insert(&d).second) return;
        require(seen.size() <= l.nodes, "Description node limit");
        active.insert(&d); f(d);
        for (const auto& child : d.orderedChildren()) { require(bool(child), "Null description"); walk(*child, depth + 1); }
        active.erase(&d);
    };
    walk(root, 0);
}
std::vector<ValidationError> validate(const Description& root, const Model& model, Limits l) {
    std::vector<ValidationError> errors;
    // Preflight the complete graph, even when a hidden container will be skipped.
    visit(root, [](const auto& d) { supported(d); }, l);
    std::size_t work = 0;
    std::function<void(const Description&, std::size_t)> walk = [&](const auto& d, auto depth) {
        require(++work <= l.visits && depth <= l.depth, "Validation traversal limit");
        if (!d.flag("visible") || d.flag("readOnly")) return;
        if (d.isContainer()) {
            require(d.conditions.empty(), "Container local conditions are not implemented");
            for (const auto& c : d.orderedChildren()) walk(*c, depth + 1);
        } else {
            auto e = validateValue(d, model.readUsing(d)); errors.insert(errors.end(), e.begin(), e.end());
        }
    };
    walk(root, 0); return errors;
}
Value decodeValue(const std::string& input, Limits l) {
    try { return value(parse(input, l), l); } catch (const J::exception& e) { throw Error(e.what()); }
}
std::string encodeValue(const Value& v, Limits l) {
    try { auto s = wire(v, l).dump(); (void)parse(s, l); return s; } catch (const J::exception& e) { throw Error(e.what()); }
}
Graph Graph::decode(const std::string& input, Limits l) {
    try {
        auto j = parse(input, l);
        keys(j, {"format", "root", "descriptions", "objects"});
        require(j.at("format") == "magritte.graph/1", "Unsupported format");
        Graph g; g.root = identity(j.at("root"));
        require(j.at("descriptions").is_array() && j.at("descriptions").size() <= l.nodes, "Description node limit");
        for (const auto& n : j.at("descriptions")) {
            auto d = std::make_shared<Description>();
            d->id = identity(n.at("id")); d->className = string(n.at("class"));
            if (d->isContainer()) keys(n, {"id", "class", "properties", "children"});
            else keys(n, {"id", "class", "properties", "accessor"});
            require(n.at("properties").is_object(), "Expected properties dictionary");
            for (auto it = n.at("properties").begin(); it != n.at("properties").end(); ++it) { (void)identity(J(it.key())); d->properties.emplace(it.key(), value(it.value(), l)); }
            if (!d->isContainer()) {
                const auto& a = n.at("accessor"); keys(a, {"class", "key"});
                require(a.at("class") == "MADictionaryAccessor", "Unsupported accessor strategy");
                d->key = identity(a.at("key"));
            }
            supported(*d);
            require(g.descriptions.emplace(d->id, d).second, "Duplicate description identity");
        }
        for (const auto& n : j.at("descriptions")) {
            auto d = g.descriptions.at(string(n.at("id")));
            if (!d->isContainer()) continue;
            require(n.at("children").is_array(), "Expected children array");
            std::set<std::string> unique;
            for (const auto& c : n.at("children")) {
                auto id = identity(c);
                require(unique.insert(id).second, "Duplicate container child");
                require(g.descriptions.count(id), "Dangling description identity");
                auto child = g.descriptions.at(id);
                // Nested containers need identity/native object access semantics in a later profile.
                require(!child->isContainer(), "Nested containers are not implemented in this profile");
                d->children.push_back(child);
            }
        }
        require(g.descriptions.count(g.root) && g.descriptions.at(g.root)->isContainer(), "Root must identify a container");
        for (const auto& p : g.descriptions) visit(*p.second, [](const auto&) {}, l);
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
        return g;
    } catch (const J::exception& e) { throw Error(e.what()); }
}
std::string Graph::encode(Limits l) const {
    try {
        J j{{"format", "magritte.graph/1"}, {"root", root}, {"descriptions", J::array()}, {"objects", J::array()}};
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
