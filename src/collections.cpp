#include <magritte/magritte.hpp>
#include "detail.hpp"
#include <algorithm>
#include <cmath>


namespace magritte {
Value::Value(Class v):data(std::move(v)){if(!detail::knownClass(std::get<Class>(data).name))throw Error("Unknown portable class");}

Value::Value(double v) : data(v) { if (!std::isfinite(v)) throw Error("Non-finite float"); }
bool Value::isCollection() const { return data.index() == 3 || data.index() == 4 || data.index() == 7 || data.index() == 8; }
std::size_t Value::collectionSize() const {
    if (auto a = std::get_if<Array>(&data)) return a->size();
    if (auto c = std::get_if<Collection>(&data)) return c->elements.size();
    if (auto s = std::get_if<Symbol>(&data)) return Value(s->utf8).collectionSize();
    if (auto s = std::get_if<std::string>(&data)) return static_cast<std::size_t>(std::count_if(s->begin(), s->end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }));
    throw Error("Value is not a collection");
}
Value::Array Value::elements() const {
    if (auto a = std::get_if<Array>(&data)) return *a;
    if (auto s = std::get_if<Symbol>(&data)) return Value(s->utf8).elements();
    if (auto c = std::get_if<Collection>(&data)) return c->elements;
    if (auto s = std::get_if<std::string>(&data)) {
        Array result;
        for (std::size_t i = 0; i < s->size();) {
            auto c = static_cast<unsigned char>((*s)[i]);
            auto n = c < 128 ? 1U : (c >= 0xc2 && c <= 0xdf) ? 2U : (c >= 0xe0 && c <= 0xef) ? 3U : (c >= 0xf0 && c <= 0xf4) ? 4U : 0U;
            if (n == 0) throw Error("Invalid UTF-8 collection");
            if (i + n > s->size()) throw Error("Invalid UTF-8 collection");
            for (unsigned j = 1; j < n; ++j) {
                auto byte = static_cast<unsigned char>((*s)[i+j]);
                if (byte < 0x80 || byte > 0xbf) throw Error("Invalid UTF-8 collection");
            }
            if (n > 1) {
                auto next = static_cast<unsigned char>((*s)[i+1]);
                if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) throw Error("Invalid Unicode scalar");
            }
            result.emplace_back(Character{s->substr(i, n)}); i += n;
        }
        return result;
    }
    throw Error("Value is not a collection");
}
namespace detail {
bool portableLabelKey(const Value& v) {
    if (v.data.index() == 0 || v.data.index() == 1 || v.data.index() == 5 || v.data.index() == 6 || v.data.index() == 8 || v.data.index() == 10) return true;
    if (auto i = std::get_if<std::int64_t>(&v.data)) return *i >= -1073741824 && *i <= 1073741823;
    return false;
}
std::string valueKey(const Value& v, Limits l, std::size_t& work, std::size_t depth) {
    if (++work > l.visits || depth > l.depth) throw Error("Value comparison work/depth limit");
    const auto wrap = [](std::string s) { return std::to_string(s.size()) + ":" + s; };
    auto hex = [&](const std::string& s) {
        if (s.size() > l.visits - work) throw Error("Text comparison work limit");
        work += s.size();
        std::string result; const char* digits = "0123456789abcdef";
        for (unsigned char c : s) { result += digits[c >> 4]; result += digits[c & 15]; }
        return result;
    };
    std::string result = std::to_string(v.data.index()) + ":";
    switch (v.data.index()) {
        case 0: return result;
        case 1: return result + (std::get<bool>(v.data) ? "1" : "0");
        case 2: return result + std::to_string(std::get<std::int64_t>(v.data));
        case 8: return "3:" + wrap(hex(std::get<Value::Symbol>(v.data).utf8));
        case 9: {
            auto n = std::get<double>(v.data);
            if (!std::isfinite(n)) throw Error("Non-finite float");
            if (n >= -9223372036854775808.0 && n < 9223372036854775808.0 && std::trunc(n) == n) return "2:" + std::to_string(static_cast<std::int64_t>(n));
            return result + floatBits(n);
        }
        case 10: {auto name=std::get<Value::Class>(v.data).name;if(!knownClass(name))throw Error("Unknown portable class");return result+wrap(hex(name));}
        case 3: return result + wrap(hex(std::get<std::string>(v.data)));
        case 5: return result + wrap(hex(std::get<Value::Reference>(v.data).id));
        case 6: return result + wrap(hex(std::get<Value::Character>(v.data).utf8));
        default: {
            std::vector<std::string> keys;
            const auto& elements = v.data.index() == 4 ? std::get<Value::Array>(v.data) : std::get<Value::Collection>(v.data).elements;
            for (const auto& e : elements) keys.push_back(valueKey(e, l, work, depth + 1));
            if (v.data.index() == 7) {
                const auto& kind = std::get<Value::Collection>(v.data).kind;
                result += wrap(kind);
                if (kind != "ordered") std::sort(keys.begin(), keys.end());
            }
            for (const auto& key : keys) result += wrap(key);
            return result;
        }
    }
}
}
bool Value::operator==(const Value& v) const {
    std::size_t work = 0;
    Limits local; local.visits = static_cast<std::size_t>(-1); local.depth = static_cast<std::size_t>(-1);
    return detail::valueKey(*this, local, work) == detail::valueKey(v, local, work);
}
bool Description::isRelation() const {
    return className == "MAToOneRelationDescription" || className == "MAToManyRelationDescription" || className == "MAToManyScalarRelationDescription";
}
bool Description::isOption() const { return className == "MASingleOptionDescription" || className == "MAMultipleOptionDescription"; }
bool Description::isMultiple() const { return className == "MAToManyRelationDescription" || className == "MAToManyScalarRelationDescription" || className == "MAMultipleOptionDescription"; }
std::shared_ptr<Description> Description::referenceDescription() const {
    auto r = reference.lock(); if (!r) throw Error("Reference description is unavailable; retain it in the graph"); return r;
}
std::string Description::labelForOption(const Value& v, Limits l) const {
    std::size_t work = 0; auto key = detail::valueKey(v, l, work);
    auto bounded = [&](const std::string& text) {
        if (text.size() > l.bytes || work > l.visits || text.size() > l.visits - work) throw Error("Option label output limit");
        work += text.size(); return text;
    };
    if (className == "MABooleanDescription") {
        if (auto b = std::get_if<bool>(&v.data)) return bounded(std::get<std::string>(property(*b ? "trueString" : "falseString").data));
        return bounded(std::get<std::string>(property("undefined").data));
    }
    if (!isOption()) throw Error("Option operation requires option description");
    for (const auto& label : labels) {
        if (!detail::portableLabelKey(label.first)) throw Error("Identity-sensitive option label key is not portable");
        if (label.first.data.index() == v.data.index() && detail::valueKey(label.first, l, work) == key) return bounded(label.second);
    }
    return bounded(detail::displayString(v));
}
Options Description::allOptionsWith(const Value& current, Limits l) const {
    if (className == "MABooleanDescription") { if(l.visits<2||l.items<2)throw Error("Boolean option item/work limit");return {Value(false),Value(true)}; }
    if (!isOption()) throw Error("Option operation requires option description");
    if (options.isDynamic()) {
        // shouldNotInclude: short-circuits before copying; dynamic sources have no copyWith:.
        if (flag("extensible") && !current.isNil() && !options.provider()->includes(current, l))
            throw Error("Dynamic options do not implement copyWith:");
        std::vector<OptionItem> items;
        if (!isMultiple() && !flag("required")) items.emplace_back(property("undefinedValue"));
        // Object>>asArray wraps the copied provider. Sorting a singleton invokes no comparator.
        items.emplace_back(options.provider()->copy());
        (void)flag("sorted");
        if (items.size() > l.items || l.visits < 1) throw Error("Options item/work limit");
        return Options::collection(std::move(items));
    }
    std::size_t work = 0;
    bool contains = false; auto key = detail::valueKey(current, l, work);
    for (const auto& option : options.values(l)) if (detail::valueKey(option, l, work) == key) contains = true;
    auto result = options.values(l);
    if (flag("extensible") && !current.isNil() && !contains) result.push_back(current);
    if (flag("sorted")) {
        auto ref = referenceDescription();
        auto sortKey = [&](const Value& v) {
            auto key = writeString(*ref, v, nullptr, l);
            for (auto& c : key) {
                if (static_cast<unsigned char>(c) > 127) throw Error("Non-ASCII case-insensitive option sorting is not implemented");
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return key;
        };
        // Check every key even for a singleton so unsupported rendering cannot hide.
        std::vector<std::pair<std::string, Value>> ranked;
        for (const auto& v : result) ranked.emplace_back(sortKey(v), v);
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        result.clear(); for (const auto& item : ranked) result.push_back(item.second);
    }
    if (!isMultiple() && !flag("required")) result.insert(result.begin(), property("undefinedValue"));
    return result;
}
Options Description::allOptions(Limits l) const {
    if (isOption() && options.isDynamic()) return options.provider();
    return allOptionsWith({}, l);
}
bool NativeModel::kindOf(const std::string& name) const { auto& registry=ClassRegistry::global();
    bool registered=std::any_of(classes_.begin(),classes_.end(),[&](const auto& cls){return registry.find(cls).has_value();});
    return (!registered && registry.includesBehavior("Object",name)) || std::any_of(classes_.begin(),classes_.end(),[&](const auto& cls){return cls==name || registry.includesBehavior(cls,name);}); }
const Model& Graph::resolve(const Value::Reference& r) const {
    const auto a = objects.find(r.id); const auto b = nativeObjects.find(r.id);
    if (a != objects.end() && b != nativeObjects.end()) throw Error("Duplicate portable/native object identity");
    if (a != objects.end()) return a->second;
    if (b != nativeObjects.end() && b->second) return *b->second;
    throw Error("Dangling object reference: " + r.id);
}
const Model& Graph::resolve(const Value& v) const {
    auto r = std::get_if<Value::Reference>(&v.data);
    if (!r) throw Error("Expected object reference"); return resolve(*r);
}
Model& Graph::resolve(const Value& v) { return const_cast<Model&>(static_cast<const Graph&>(*this).resolve(v)); }
} // namespace magritte
