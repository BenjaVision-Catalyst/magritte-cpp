#include <magritte/magritte.hpp>
#include "detail.hpp"
#include <algorithm>


namespace magritte {
bool Value::isCollection() const { return data.index() == 3 || data.index() == 4 || data.index() == 7; }
std::size_t Value::collectionSize() const {
    if (auto a = std::get_if<Array>(&data)) return a->size();
    if (auto c = std::get_if<Collection>(&data)) return c->elements.size();
    if (auto s = std::get_if<std::string>(&data)) return static_cast<std::size_t>(std::count_if(s->begin(), s->end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }));
    throw Error("Value is not a collection");
}
Value::Array Value::elements() const {
    if (auto a = std::get_if<Array>(&data)) return *a;
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
    if (v.data.index() == 0 || v.data.index() == 1 || v.data.index() == 5 || v.data.index() == 6) return true;
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
namespace {
std::string text(const Value& v, const Description& ref, Limits l, std::size_t& work) {
    if (detail::valueKey(v, l, work) == detail::valueKey(ref.property("undefinedValue"), l, work)) {
        auto undefined = ref.property("undefined");
        if (auto s = std::get_if<std::string>(&undefined.data)) return *s;
        return "";
    }
    if (auto s = std::get_if<std::string>(&v.data)) return *s;
    if (auto c = std::get_if<Value::Character>(&v.data)) return c->utf8;
    if (auto i = std::get_if<std::int64_t>(&v.data)) return std::to_string(*i);
    if (auto b = std::get_if<bool>(&v.data)) return *b ? "true" : "false";
    throw Error("Unlabelled complex option rendering is not portable");
}
}
std::string Description::labelForOption(const Value& v, Limits l) const {
    std::size_t work = 0; auto key = detail::valueKey(v, l, work);
    if (!isOption()) throw Error("Option operation requires option description");
    for (const auto& label : labels) {
        if (!detail::portableLabelKey(label.first)) throw Error("Identity-sensitive option label key is not portable");
        if (detail::valueKey(label.first, l, work) == key) return label.second;
    }
    return text(v, *referenceDescription(), l, work);
}
std::vector<Value> Description::allOptionsWith(const Value& current, Limits l) const {
    std::size_t work = 0;
    bool contains = false; auto key = detail::valueKey(current, l, work);
    for (const auto& option : options) if (detail::valueKey(option, l, work) == key) contains = true;
    if (!isOption()) throw Error("Option operation requires option description");
    auto result = options;
    if (flag("extensible") && !current.isNil() && !contains) result.push_back(current);
    if (flag("sorted")) {
        auto ref = referenceDescription();
        auto sortKey = [&](const Value& v) {
            auto key = text(v, *ref, l, work);
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
std::vector<Value> Description::allOptions(Limits l) const { return allOptionsWith({}, l); }
bool NativeModel::kindOf(const std::string& name) const { return name == "Object" || std::find(classes_.begin(), classes_.end(), name) != classes_.end(); }
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
} // namespace magritte
