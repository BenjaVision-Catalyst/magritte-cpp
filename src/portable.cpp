#include <random>
#include "detail.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <magritte/magritte.hpp>

#include <set>

namespace magritte {
namespace {
void need(bool b, const std::string &s) {
    if (!b)
        throw Error(s);
}
struct Work {
    Limits l;
    std::size_t n = 0;
    void tick(std::size_t depth, std::size_t amount = 1) {
        need(depth <= l.depth && n <= l.visits && amount <= l.visits - n,
             "Portable interpreter work/depth limit");
        n += amount;
    }
};
std::string text(const Value &v) {
  if (v.isMutableCollection() && v.mutableCollection()->kind() == "string")
    return text(v.mutableCollection()->snapshot());
  if (auto s = std::get_if<std::string>(&v.data))
    return *s;
  if (auto s = std::get_if<Value::Symbol>(&v.data))
    return s->utf8;
  throw Error("Expected text");
}
std::size_t arity(const std::string &s) {
    static const std::set<std::string> unary{"isNil",    "notNil",   "isEmpty", "notEmpty", "isInteger",
                                             "positive", "negative", "even",    "odd"};
    static const std::set<std::string> binary{"=",  "~=",        "<",           "<=",       ">",
                                              ">=", "includes:", "beginsWith:", "endsWith:"};
    if (unary.count(s))
        return 0;
    if (binary.count(s))
        return 1;
    if (s == "between:and:")
        return 2;
    throw Error("Unsupported portable condition selector: " + s);
}
void checkPredicate(const Predicate &p, Work &w, std::size_t depth) {
    w.tick(depth);
    if (p.className == "MAPluggableCondition") {
        need(p.arguments.size() == arity(p.selector) && p.index <= p.arguments.size() && p.children.empty(),
             "Invalid pluggable condition shape");
        // Constants cannot carry object identities: their message protocol is value-only.
        std::function<void(const Value &, std::size_t)> constant = [&](const Value &v, std::size_t depth) {
            w.tick(depth);
            need(v.data.index() != 5, "Condition object constants are unsupported");
            if (v.data.index() == 4 || v.data.index() == 7)
                for (const auto &e : v.elements())
                    constant(e, depth + 1);
        };
        auto checkValue = [&](const Value &v) {
            (void)detail::valueKey(v, w.l, w.n);
            (void)encodeValue(v, w.l);
            constant(v, depth + 1);
        };
        checkValue(p.receiver);
        for (const auto &v : p.arguments)
            checkValue(v);
    } else {
        need(p.selector.empty() && p.arguments.empty() && p.index == 0 && p.receiver.isNil(),
             "Invalid compound condition shape");
        need(p.className == "MAConjunctiveCondition" ||
                 (p.className == "MANegationCondition" && p.children.size() == 1),
             "Unsupported compound condition");
        for (const auto &child : p.children)
            checkPredicate(child, w, depth + 1);
    }
}
bool predicate(const Predicate &p, const Value &input, Work &w, std::size_t depth) {
    w.tick(depth);
    if (p.className == "MANegationCondition")
        return !predicate(p.children.front(), input, w, depth + 1);
    if (p.className == "MAConjunctiveCondition") {
        for (const auto &c : p.children)
            if (!predicate(c, input, w, depth + 1))
                return false;
        return true;
    }
    auto r = p.index == 0 ? input : p.receiver;
    auto args = p.arguments;
    if (p.index)
        args[p.index - 1] = input;
    auto equal = [&](const Value &a, const Value &b) {
      return detail::valuesEqual(a, b, w.l, w.n);
    };
    const auto &s = p.selector;
    if (s == "isNil")
        return r.isNil();
    if (s == "notNil")
        return !r.isNil();
    if (s == "=")
        return equal(r, args[0]);
    if (s == "~=")
        return !equal(r, args[0]);
    if (s == "isInteger")
        return r.data.index() == 2;
    if (s == "isEmpty" || s == "notEmpty") {
        auto empty = r.collectionSize() == 0;
        return s == "isEmpty" ? empty : !empty;
    }
    if (s == "includes:") {
        need(r.collectionSize() <= w.l.visits - w.n, "Condition collection work limit");
        for (const auto &e : r.elements()) {
            w.tick(depth);
            if (equal(e, args[0]))
                return true;
        }
        return false;
    }
    if (s == "beginsWith:" || s == "endsWith:") {
        auto a = text(r), b = text(args[0]);
        w.tick(depth, a.size() + b.size());
        if (b.size() > a.size())
            return false;
        return a.compare(s == "beginsWith:" ? 0 : a.size() - b.size(), b.size(), b) == 0;
    }
    if (s == "even" || s == "odd") {
        need(detail::isNumber(r) && r.data.index() != 12,
             "Parity requires supported numeric receiver");
        auto even = r.data.index() == 2 ? std::get<std::int64_t>(r.data) % 2 == 0
                                        : std::fmod(std::get<double>(r.data), 2.0) == 0;
        return s == "even" ? even : !even;
    }
    if (s == "positive" || s == "negative") {
        auto c = detail::compareNumbers(r, Value(std::int64_t(0)));
        return s == "positive" ? c >= 0 : c < 0;
    }
    auto cmp = [&](const Value &a, const Value &b) {
        if (detail::isNumber(a) && detail::isNumber(b))
            return detail::compareNumbers(a, b);
        auto x = text(a), y = text(b);
        w.tick(depth, x.size() + y.size());
        return x < y ? -1 : x > y ? 1 : 0;
    };
    if (s == "between:and:")
        return cmp(r, args[0]) >= 0 && cmp(r, args[1]) <= 0;
    auto c = cmp(r, args[0]);
    return s == "<" ? c < 0 : s == "<=" ? c <= 0 : s == ">" ? c > 0 : c >= 0;
}
void checkAccessor(const Accessor &a, Work &w, std::size_t depth) {
    w.tick(depth);
    need(a.className == "MACompositeAccessor" || !a.kind, "Unexpected accessor kind");
    if (a.kind)
        need(detail::knownClass(*a.kind), "Unknown composite kind");
    need(a.className == "MAPluggableAccessor" || (!a.readBlock && !a.writeBlock),
         "Blocks require a pluggable accessor");
    if (a.className == "MAPluggableAccessor") {
        need(a.name.empty() && a.writeName.empty() && a.uuid.isNil() && a.steps.empty(),
             "Invalid pluggable accessor shape");
        need(!a.readBlock || static_cast<bool>(*a.readBlock), "Empty read callable");
        need(!a.writeBlock || static_cast<bool>(*a.writeBlock), "Empty write callable");
        return;
    }
    need(a.className == "MANullAccessor" || a.uuid.isNil(), "Unexpected null accessor token");
    (void)detail::valueKey(a.uuid, w.l, w.n);
    if (!a.uuid.isNil()) {
        need(a.uuid.data.index() == 4 && a.uuid.collectionSize() == 16, "Null UUID requires 16 bytes or nil");
        for (const auto &e : a.uuid.elements())
            need(e.data.index() == 2 && std::get<std::int64_t>(e.data) >= 0 &&
                     std::get<std::int64_t>(e.data) <= 255,
                 "Invalid UUID byte");
    }
    if (a.className == "MADelegatorAccessor") { need(a.steps.size()==1 && a.name.empty() && a.writeName.empty(),"Invalid delegator accessor");checkAccessor(a.steps.front(),w,depth+1);return; }
    if (a.className == "MAChainAccessor" || a.className == "MACompositeAccessor") {
        need(a.className != "MACompositeAccessor" || a.steps.size() == 2,
             "Composite requires exactly head and next");
        need(a.steps.size() >= 2 && a.name.empty() && a.writeName.empty(), "Invalid chain accessor");
        for (const auto &step : a.steps)
            checkAccessor(step, w, depth + 1);
        return;
    }
    need(a.steps.empty(), "Nonchain accessor has steps");
    if (a.className == "MAIdentityAccessor" || a.className == "MANullAccessor") {
        need(a.name.empty() && a.writeName.empty(), "Unexpected accessor name");
        return;
    }
    need(a.className == "MADictionaryAccessor" || a.className == "MAPropertyAccessor" ||
             a.className == "MASelectorAccessor" || a.className == "MAVariableAccessor",
         "Unsupported accessor");
    need(a.className == "MASelectorAccessor" || !a.name.empty(), "Missing accessor name");
    w.tick(depth, a.name.size() + a.writeName.size());
    need(a.className == "MASelectorAccessor" || a.writeName.empty(), "Unexpected write selector");
}
Value identity(const Model &m, const Graph *g, Work &w) {
    need(g, "Identity value access requires graph registration");
    for (const auto &p : g->objects) {
        w.tick(0);
        if (&p.second == &m)
            return Value(Value::Reference{p.first});
    }
    for (const auto &p : g->nativeObjects) {
        w.tick(0);
        if (p.second.get() == &m)
            return Value(Value::Reference{p.first});
    }
    throw Error("Identity target is unregistered");
}
Value accessRead(const Accessor &a, const Model &m, const Graph *g, Work &w, std::size_t depth) {
    w.tick(depth);
    if (a.className == "MAPluggableAccessor") {
        auto block = a.readBlock;
        need(static_cast<bool>(block), "Pluggable accessor read block is missing");
        auto result = (*block)(m);
        detail::checkLocalValue(result, w.l);
        return result;
    }
    if (a.className == "MACompositeAccessor") {
        auto elements = accessRead(a.steps[0], m, g, w, depth + 1);
        need(elements.collectionSize() <= w.l.items, "Composite item limit");
        Value::Array values;
        for (const auto &e : elements.elements()) {
            w.tick(depth);
            need(g, "Composite traversal requires graph resolver");
            values.push_back(accessRead(a.steps[1], g->resolve(e), g, w, depth + 1));
        }
        std::set<std::string> keys;
        for (const auto &v : values)
            keys.insert(detail::valueKey(v, w.l, w.n, depth + 1));
        if (keys.size() > 1) {
            auto c = std::make_shared<CompositeConflict>();
            c->kind = a.kind;
            return Value(c);
        }
        need(!values.empty(), "Composite read of empty collection");
        return values.front();
    }
    if (a.className == "MANullAccessor")
        throw Error("MAReadError: null accessor");
    if (a.className == "MAIdentityAccessor")
        return identity(m, g, w);
    if(a.className=="MADelegatorAccessor")return accessRead(a.steps.front(),m,g,w,depth+1);
    if (a.className != "MAChainAccessor") {
        need(a.className != "MASelectorAccessor" || !a.name.empty(), "Absent read selector");
        return m.readSlot(a.className, a.name);
    }
    const Model *target = &m;
    Value result;
    for (std::size_t i = 0; i < a.steps.size(); ++i) {
        result = accessRead(a.steps[i], *target, g, w, depth + 1);
        if (i + 1 < a.steps.size()) {
            need(g, "Chain traversal requires graph resolver");
            target = &g->resolve(result);
        }
    }
    return result;
}
Value accessReadValue(const Accessor &a, const Value &value, const Graph *graph, Work &work,
                      std::size_t depth) {
    work.tick(depth);
    if (a.className == "MAIdentityAccessor")
        return value;
    if (a.className == "MADelegatorAccessor")
        return accessReadValue(a.steps.front(), value, graph, work, depth + 1);
    if (a.className == "MAChainAccessor") {
        auto result = value;
        for (const auto &step : a.steps)
            result = accessReadValue(step, result, graph, work, depth + 1);
        return result;
    }
    need(graph, "Value accessor requires graph resolver");
    return accessRead(a, graph->resolve(value), graph, work, depth + 1);
}
void accessWrite(const Accessor &a, Model &m, Value v, Graph *g, Work &w, std::size_t depth) {
    w.tick(depth);
    if (a.className == "MAPluggableAccessor") {
        detail::checkLocalValue(v, w.l);
        auto block = a.writeBlock;
        need(static_cast<bool>(block), "Pluggable accessor write block is missing");
        (*block)(m, v);
        return;
    }
    if (a.className == "MACompositeAccessor") {
        auto elements = accessRead(a.steps[0], m, g, w, depth + 1);
        need(elements.collectionSize() <= w.l.items, "Composite item limit");
        for (const auto &e : elements.elements()) {
            w.tick(depth);
            if (v.conflict())
                continue;
            need(g, "Composite traversal requires graph resolver");
            accessWrite(a.steps[1], g->resolve(e), v, g, w, depth + 1);
        }
        return;
    }
    if (a.className == "MANullAccessor" || a.className == "MAIdentityAccessor")
        throw Error("MAWriteError: read-only accessor");
    if(a.className=="MADelegatorAccessor"){accessWrite(a.steps.front(),m,std::move(v),g,w,depth+1);return;}
    if (a.className != "MAChainAccessor") {
        need(a.className != "MASelectorAccessor" || !a.writeName.empty(), "Absent write selector");
        m.writeSlot(a.className, a.className == "MASelectorAccessor" ? a.writeName : a.name, std::move(v));
        return;
    }
    Model *target = &m;
    for (std::size_t i = 0; i + 1 < a.steps.size(); ++i) {
        auto next = accessRead(a.steps[i], *target, g, w, depth + 1);
        need(g, "Chain traversal requires graph resolver");
        target = &g->resolve(next);
    }
    accessWrite(a.steps.back(), *target, std::move(v), g, w, depth + 1);
}
// Resolve an intermediate result only when the next capability needs a model.
// Dictionary/property/identity/null capabilities are independent of their receiver.
struct CapabilityTarget {
    const Model *model;
    const Value *value = nullptr;
    const Model &resolve(const Graph *g) const {
        if (model)
            return *model;
        need(g, "Chain capability traversal requires graph resolver");
        return g->resolve(*value);
    }
};
bool accessCan(const Accessor &a, CapabilityTarget target, const Graph *g, Work &w,
               std::size_t depth, bool writing) {
    w.tick(depth);
    if (a.className == "MAPluggableAccessor")
        return writing ? bool(a.writeBlock) : bool(a.readBlock);
    if (a.className == "MADictionaryAccessor" || a.className == "MAPropertyAccessor")
        return true;
    if (a.className == "MANullAccessor")
        return false;
    if (a.className == "MAIdentityAccessor")
        return !writing;
    if (a.className == "MASelectorAccessor") {
        const auto &selector = writing ? a.writeName : a.name;
        return !selector.empty() && target.resolve(g).respondsToSelector(selector);
    }
    if (a.className == "MAVariableAccessor")
        return target.resolve(g).hasVariable(a.name);
    if (a.className == "MADelegatorAccessor")
        return accessCan(a.steps.front(), target, g, w, depth + 1, writing);
    if (a.className == "MACompositeAccessor") {
        if (!accessCan(a.steps[0], target, g, w, depth + 1, false))
            return false;
        auto elements = accessRead(a.steps[0], target.resolve(g), g, w, depth + 1);
        need(elements.collectionSize() <= w.l.items, "Composite item limit");
        for (const auto &e : elements.elements()) {
            w.tick(depth);
            if (!accessCan(a.steps[1], {nullptr, &e}, g, w, depth + 1, writing))
                return false;
        }
        return true;
    }
    Value next;
    for (std::size_t i = 0; i + 1 < a.steps.size(); ++i) {
        if (!accessCan(a.steps[i], target, g, w, depth + 1, false))
            return false;
        next = accessRead(a.steps[i], target.resolve(g), g, w, depth + 1);
        target = {nullptr, &next};
    }
    return accessCan(a.steps.back(), target, g, w, depth + 1, writing);
}
std::string boundedJoin(const std::string &left, const std::string &right, Work &w,
                        std::size_t depth) {
    need(left.size() <= w.l.bytes && right.size() <= w.l.bytes && w.l.bytes - left.size() >= 2 &&
             w.l.bytes - left.size() - 2 >= right.size(),
         "Accessor text byte limit");
    w.tick(depth, left.size() + 2 + right.size());
    return left + "::" + right;
}
Value inferredName(const Accessor &a, Work &w, std::size_t depth) {
    w.tick(depth);
    if (a.className == "MAIdentityAccessor" || a.className == "MANullAccessor" ||
        a.className == "MAPluggableAccessor")
        return {};
    if (a.className == "MADelegatorAccessor")
        return inferredName(a.steps.front(), w, depth + 1);
    if (a.className == "MAChainAccessor" || a.className == "MACompositeAccessor") {
        auto result = inferredName(a.steps.back(), w, depth + 1);
        for (std::size_t i = a.steps.size() - 1; i > 0; --i) {
            const bool symbol = result.data.index() == 8;
            auto head = inferredName(a.steps[i - 1], w, depth + 1);
            auto joined = boundedJoin(text(result), text(head), w, depth);
            result = symbol ? Value(Value::Symbol{joined}) : Value(joined);
        }
        return result;
    }
    if (a.className == "MASelectorAccessor" && a.name.empty())
        return {};
    return a.className == "MAVariableAccessor" ? Value(a.name) : Value(Value::Symbol{a.name});
}
std::string inferredLabel(const Accessor &a, Work &w, std::size_t depth) {
    w.tick(depth);
    if (a.className == "MAIdentityAccessor" || a.className == "MANullAccessor" ||
        a.className == "MAVariableAccessor" || a.className == "MAPluggableAccessor")
        return "";
    if (a.className == "MADelegatorAccessor")
        return inferredLabel(a.steps.front(), w, depth + 1);
    if (a.className == "MAChainAccessor" || a.className == "MACompositeAccessor") {
        auto result = inferredLabel(a.steps.front(), w, depth + 1);
        for (std::size_t i = 1; i < a.steps.size(); ++i)
            result = boundedJoin(result, inferredLabel(a.steps[i], w, depth + 1), w, depth);
        return result;
    }
    const auto source = a.className == "MASelectorAccessor" && a.name.empty() ? "nil" : a.name;
    std::string result;
    auto upper = [](unsigned char c) { return c >= 'A' && c <= 'Z'; };
    for (unsigned char c : source) {
        need(c < 128, "Non-ASCII inferred labels require an explicit label");
        if (upper(c) && !result.empty() && !upper(static_cast<unsigned char>(result.back()))) {
            w.tick(depth);
            need(result.size() < w.l.bytes, "Accessor text byte limit");
            result += ' ';
        }
        w.tick(depth);
        need(result.size() < w.l.bytes, "Accessor text byte limit");
        result += static_cast<char>(c);
    }
    if (!result.empty() && result.front() >= 'a' && result.front() <= 'z')
        result.front() += 'A' - 'a';
    return result;
}
std::string propText(const Description &d, const std::string &name) {
    auto v = d.property(name);
    return v.isNil() ? "" : text(v);
}
Value read(const Description &d, const std::string &input, Work &w, std::size_t depth, bool initial) {
    w.tick(depth, input.size() + 1);
    auto undef = propText(d, "undefined");
    if (initial && (input.empty() || (!undef.empty() && input.rfind(undef, 0) == 0)))
        return {};
    if (detail::descriptionIs(d, "MASymbolDescription"))
        return Value(Value::Symbol{input});
    if (detail::descriptionIs(d, "MAStringDescription"))
        return Value(input);
    if (detail::descriptionIs(d, "MABooleanDescription")) {
        auto lower = [](std::string s) {
            for (auto &c : s) {
                need(static_cast<unsigned char>(c) < 128, "Non-ASCII Boolean alias comparison unsupported");
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            }
            return s;
        };
        for (const auto &pair :
             {std::make_pair("trueString", "trueStrings"), std::make_pair("falseString", "falseStrings")}) {
            bool yes = std::string(pair.first) == "trueString";
            if (input == propText(d, pair.first))
                return Value(yes);
            for (const auto &alias : d.property(pair.second).elements()) {
                auto s = text(alias);
                w.tick(depth, s.size());
                if (lower(s) == lower(input))
                    return Value(yes);
            }
        }
        throw Error("MAReadError: Boolean text");
    }
    if (detail::descriptionIs(d, "MANumberDescription")) {
        auto separator = d.property("thousandsSeparator");
        auto sep = std::get<Value::Character>(separator.data).utf8;
        std::string s = input;
        for (std::size_t p; (p = s.find(sep)) != std::string::npos;)
            s.erase(p, sep.size());
        std::size_t i = !s.empty() && s[0] == '-' ? 1 : 0;
        auto digits = [&] {
            auto start = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                ++i;
            return i > start;
        };
        need(digits(), "MAReadError: numeric syntax");
        bool fraction = false;
        std::string exponent;
        if (i < s.size() && s[i] == '.') {
            fraction = true;
            ++i;
            need(digits(), "MAReadError: numeric fraction");
        }
        if (i < s.size() && s[i] == 'e') {
            ++i;
            auto start = i;
            if (i < s.size() && s[i] == '-')
                ++i;
            need(digits(), "MAReadError: numeric exponent");
            exponent = s.substr(start, i - start);
        }
        need(i == s.size(), "MAReadError: numeric syntax outside portable decimal grammar");
        if (!fraction && (exponent.empty() || exponent[0] != '-')) {
            auto integer = s.substr(0, s.find('e'));
            std::size_t zeros = 0;
            if (!exponent.empty()) {
                auto r = std::from_chars(exponent.data(), exponent.data() + exponent.size(), zeros);
                need(r.ec == std::errc{} && zeros <= 19, "MAReadError: exponent outside Int64");
            }
            integer += std::string(zeros, '0');
            std::int64_t n;
            auto r = std::from_chars(integer.data(), integer.data() + integer.size(), n);
            need(r.ec == std::errc{}, "MAReadError: integer outside Int64");
            return Value(n);
        }
        double n;
        auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        need(r.ec == std::errc{} && std::isfinite(n), "MAReadError: float outside finite binary64");
        return Value(n);
    }
    if (detail::descriptionIs(d, "MASingleOptionDescription")) {
        const Value *matched = nullptr;
        for (const auto &label : d.labels) {
            w.tick(depth, label.second.size() + 1);
            if (!label.second.empty() && input.rfind(label.second, 0) == 0) {
                need(!matched, "Ambiguous option label prefix");
                matched = &label.first;
            }
        }
        if (matched)
            return *matched;
        return read(*d.referenceDescription(), input, w, depth + 1, false);
    }
    if (detail::descriptionIs(d, "MATokenDescription") || detail::descriptionIs(d, "MAMultipleOptionDescription")) {
        Value::Array result;
        if (detail::descriptionIs(d, "MATokenDescription")) {
            auto separators = Value(propText(d, "separators")).elements();
            std::string token;
            for (const auto &c : Value(input).elements()) {
                w.tick(depth);
                if (std::find(separators.begin(), separators.end(), c) == separators.end())
                    token += std::get<Value::Character>(c.data).utf8;
                else if (!token.empty()) {
                    result.push_back(read(*d.referenceDescription(), token, w, depth + 1, true));
                    token.clear();
                }
            }
            if (!token.empty())
                result.push_back(read(*d.referenceDescription(), token, w, depth + 1, true));
        } else
            for (std::size_t start = 0; start < input.size();) {
                auto end = input.find(',', start);
                if (end == std::string::npos)
                    end = input.size();
                result.push_back(read(*d.referenceDescription(), input.substr(start, end - start),
                                      w, depth + 1, true));
                start = end + 1;
                if (start < input.size() && input[start] == ' ')
                    ++start;
            }
        return Value(result);
    }
    throw Error("MAReadError: unsupported string reader description");
}
std::string write(const Description &, const Value &, const Graph *, Work &, std::size_t, bool);
std::string writeModel(const Description &d, const Model &model, const Graph *g, Work &w, std::size_t depth) {
    w.tick(depth);
    need(d.isContainer(), "Model writer requires container");
    std::string result;
    bool first = true;
    for (const auto &child : d.orderedChildren()) {
        if (!first)
            result += '-';
        first = false;
        if (!child->flag("visible"))
            continue;
        if (child->isContainer() && !model.usesStoredContainerValues() && !child->nativeRead &&
            ((!child->accessor && !child->accessorObject) || child->effectiveAccessor().className == "MAIdentityAccessor"))
            result += writeModel(*child, model, g, w, depth + 1);
        else
            result += write(*child, model.readUsing(*child), g, w, depth + 1, true);
        need(result.size() <= w.l.bytes, "String output byte limit");
    }
    return result;
}
std::string write(const Description &d, const Value &v, const Graph *g, Work &w, std::size_t depth,
                  bool initial) {
  if (v.isMutableCollection() && v.mutableCollection()->kind() == "string")
    return write(d, v.mutableCollection()->snapshot(), g, w, depth + 1,
                 initial);
  w.tick(depth);
  std::string result;
  if (initial && v.isNil())
    result = propText(d, "undefined");
  else if (d.isContainer()) {
    need(g, "Container writer requires graph resolver");
    result = writeModel(d, g->resolve(v), g, w, depth + 1);
  } else if (detail::descriptionIs(d, "MAClassDescription")) {
    auto cls = std::get_if<Value::Class>(&v.data);
    need(cls && detail::knownClass(cls->name),
         "Class writer requires a portable Class");
    result = d.displayStringFor(v, g, w.l);
  } else if (detail::descriptionIs(d, "MABooleanDescription")) {
    need(v.data.index() == 1, "Boolean writer requires Boolean");
    result = propText(d, std::get<bool>(v.data) ? "trueString" : "falseString");
  } else if (detail::descriptionIs(d, "MASingleOptionDescription")) {
    bool found = false;
    for (const auto &label : d.labels) {
      w.tick(depth);
      need(detail::portableLabelKey(label.first), "Nonportable label key");
      if (v.data.index() == label.first.data.index() &&
          detail::valueKey(v, w.l, w.n) ==
              detail::valueKey(label.first, w.l, w.n)) {
        result = label.second;
        found = true;
        break;
      }
    }
    if (!found)
      result = d.displayStringFor(v, g, w.l);
  } else if (detail::descriptionIs(d, "MATokenDescription") || d.isMultiple()) {
    need(v.isCollection(), "Collection writer requires collection");
    need(v.collectionSize() <= w.l.visits - w.n,
         "Writer collection work limit");
    std::string sep = ", ";
    if (detail::descriptionIs(d, "MATokenDescription") &&
        v.collectionSize() > 1) {
      auto chars = Value(propText(d, "separators")).elements();
      need(!chars.empty(), "Token writer requires separator");
      sep = std::get<Value::Character>(chars.front().data).utf8;
    }
    if (detail::descriptionIs(d, "MAMultipleOptionDescription") &&
        v.collectionSize() == 0)
      result = propText(d, "undefined");
    else {
      bool first = true;
      for (const auto &e : v.elements()) {
        if (!first)
          result += sep;
        first = false;
        if (d.isOption()) {
          auto one = d;
          one.className = "MASingleOptionDescription";
          result += write(one, e, g, w, depth + 1, false);
        } else
          result += write(*d.referenceDescription(), e, g, w, depth + 1,
                          !detail::descriptionIs(d, "MATokenDescription"));
        need(result.size() <= w.l.bytes, "String output byte limit");
      }
    }
    } else if (d.isRelation())
        result = write(*d.referenceDescription(), v, g, w, depth + 1, true);
    else
        result = d.displayStringFor(v, g, w.l);
    w.tick(depth, result.size());
    need(result.size() <= w.l.bytes, "String output byte limit");
    return result;
}
} // namespace
namespace detail {
void checkPortableDescription(const Description &d, Limits l, std::size_t &work) {
    Work w{l, work};
    w.tick(0);
    if(d.accessorObject) checkAccessor(d.effectiveAccessor(),w,0);
    else if (d.accessor)
        checkAccessor(*d.accessor, w, 0);
    for (const auto &c : d.portableConditions)
        checkPredicate(c.predicate, w, 0);
    work = w.n;
}
bool evaluatePredicate(const Predicate &p, const Value &v, Limits l, std::size_t &work) {
    Work w{l, work};
    checkPredicate(p, w, 0);
    detail::checkRuntimeValue(v, l, w.n);
    auto result = predicate(p, v, w, 0);
    work = w.n;
    return result;
}
bool isNumber(const Value &v) {
    return v.data.index() == 2 || v.data.index() == 9 || v.data.index() == 12;
}
int compareNumbers(const Value &a, const Value &b) {
    need(isNumber(a) && isNumber(b), "Numeric comparison requires numbers");
    if (a.data.index() == 12 || b.data.index() == 12)
        return compareFractions(a, b);
    if (a.data.index() == 2 && b.data.index() == 2) {
        auto x = std::get<std::int64_t>(a.data), y = std::get<std::int64_t>(b.data);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (a.data.index() == 9 && b.data.index() == 2)
        return -compareNumbers(b, a);
    if (a.data.index() == 2) {
        auto x = std::get<std::int64_t>(a.data);
        auto y = std::get<double>(b.data);
        need(std::isfinite(y), "Non-finite float");
        if (y >= 9223372036854775808.0)
            return -1;
        if (y < -9223372036854775808.0)
            return 1;
        auto truncated = static_cast<std::int64_t>(y);
        if (x != truncated)
            return x < truncated ? -1 : 1;
        return y > static_cast<double>(truncated) ? -1 : y < static_cast<double>(truncated) ? 1 : 0;
    }
    auto x = std::get<double>(a.data), y = std::get<double>(b.data);
    need(std::isfinite(x) && std::isfinite(y), "Non-finite float");
    return x < y ? -1 : x > y ? 1 : 0;
}
std::string floatBits(double n) {
    need(std::isfinite(n), "Non-finite float");
    std::uint64_t bits;
    std::memcpy(&bits, &n, 8);
    std::string s(16, '0');
    const char *digits = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        s[i] = digits[bits & 15];
        bits >>= 4;
    }
    return s;
}
double floatFromBits(const std::string &s) {
    need(s.size() == 16, "Expected 16 hex float bits");
    std::uint64_t bits = 0;
    for (char c : s) {
        need((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "Noncanonical float bits");
        bits = (bits << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    double n;
    std::memcpy(&n, &bits, 8);
    need(std::isfinite(n), "Non-finite float");
    return n;
}
std::string displayString(const Value &v) {
  if (v.isMutableCollection() && v.mutableCollection()->kind() == "string")
    return displayString(v.mutableCollection()->snapshot());
  if (v.data.index() == 12) {
    auto f = detail::fractionOf(v);
    return f.denominator == 1 ? std::to_string(f.numerator)
                              : "(" + std::to_string(f.numerator) + "/" +
                                    std::to_string(f.denominator) + ")";
  }
    if (auto c = v.conflict())
        return c->string();
    if (v.isNil())
        return "nil";
    if (auto s = std::get_if<std::string>(&v.data))
        return *s;
    if (auto s = std::get_if<Value::Symbol>(&v.data))
        return s->utf8;
    if(auto cls=std::get_if<Value::Class>(&v.data))return cls->name;
    if (auto s = std::get_if<Value::Character>(&v.data))
        return s->utf8;
    if (auto i = std::get_if<std::int64_t>(&v.data))
        return std::to_string(*i);
    if (auto b = std::get_if<bool>(&v.data))
        return *b ? "true" : "false";
    if (auto n = std::get_if<double>(&v.data))
        return floatText(*n);
    throw Error("Unsupported display string");
}
std::string floatText(double n) {
    need(std::isfinite(n), "Non-finite float");
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), n, std::chars_format::scientific);
    need(r.ec == std::errc{}, "Float formatting failed");
    std::string s(buf, r.ptr);
    auto e = s.find('e');
    int exponent = std::stoi(s.substr(e + 1));
    auto mantissa = s.substr(0, e);
    if (mantissa.find('.') == std::string::npos)
        mantissa += ".0";
    if (exponent >= 6 || exponent <= -5)
        return mantissa + "e" + std::to_string(exponent);
    bool neg = mantissa[0] == '-';
    auto digits = mantissa.substr(neg ? 1 : 0);
    digits.erase(digits.find('.'), 1);
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();
    int point = exponent + 1;
    std::string out;
    if (point <= 0)
        out = "0." + std::string(-point, '0') + digits;
    else if (static_cast<std::size_t>(point) >= digits.size())
        out = digits + std::string(point - digits.size(), '0') + ".0";
    else
        out = digits.substr(0, point) + "." + digits.substr(point);
    return (neg ? "-" : "") + out;
}
} // namespace detail
void Predicate::check(Limits l) const {
    Work w{l};
    checkPredicate(*this, w, 0);
}
bool Predicate::evaluate(const Value &v, Limits l) const {
    Work w{l};
    checkPredicate(*this, w, 0);
    detail::checkRuntimeValue(v, l, w.n);
    return predicate(*this, v, w, 0);
}
void Accessor::check(Limits l) const {
    Work w{l};
    checkAccessor(*this, w, 0);
}
Value Accessor::readValue(const Value &value, const Graph *graph, Limits limits) const {
    Work work{limits};
    checkAccessor(*this, work, 0);
    detail::checkLocalValue(value, limits);
    auto result = accessReadValue(*this, value, graph, work, 0);
    detail::checkLocalValue(result, limits);
    return result;
}
Value Accessor::read(const Model &m, const Graph *g, Limits l) const {
    Work w{l};
    checkAccessor(*this, w, 0);
    return accessRead(*this, m, g, w, 0);
}
void Accessor::write(Model &m, Value v, Graph *g, Limits l) const {
    Work w{l};
    checkAccessor(*this, w, 0);
    accessWrite(*this, m, std::move(v), g, w, 0);
}
Value Accessor::accessorName(Limits limits) const {
    Work work{limits};
    checkAccessor(*this, work, 0);
    auto result = inferredName(*this, work, 0);
    (void)encodeValue(result, limits);
    return result;
}
std::string Accessor::defaultLabelFor(const Description &, Limits limits) const {
    Work work{limits};
    checkAccessor(*this, work, 0);
    return inferredLabel(*this, work, 0);
}
std::string Description::defaultLabel(Limits limits) const {
    return effectiveAccessor(limits).defaultLabelFor(*this, limits);
}
bool Description::hasLabel(Limits limits) const {
    const auto label = property("label", limits);
    return !label.isNil() && !text(label).empty();
}
Accessor Accessor::pluggable(std::shared_ptr<const ReadBlock> read,
                             std::shared_ptr<const WriteBlock> write) {
    Accessor result;
    result.className = "MAPluggableAccessor";
    result.readBlock = std::move(read);
    result.writeBlock = std::move(write);
    return result;
}
bool Accessor::canRead(const Model &m, const Graph *g, Limits l) const {
    Work w{l};
    checkAccessor(*this, w, 0);
    return accessCan(*this, {&m}, g, w, 0, false);
}
bool Accessor::canWrite(const Model &m, const Graph *g, Limits l) const {
    Work w{l};
    checkAccessor(*this, w, 0);
    return accessCan(*this, {&m}, g, w, 0, true);
}
bool Description::respondsToSelector(const std::string &selector) const {
    const bool writing = !selector.empty() && selector.back() == ':';
    return detail::metadataSelector(
        *this, writing ? selector.substr(0, selector.size() - 1) : selector, writing);
}
bool NativeModel::respondsToSelector(const std::string &selector) const {
    const auto key = "MASelectorAccessor:" + selector;
    const auto r = readers.find(key);
    const auto w = writers.find(key);
    return object_ && ((r != readers.end() && r->second) || (w != writers.end() && w->second));
}
bool NativeModel::hasVariable(const std::string &name) const {
    const auto key = "MAVariableAccessor:" + name;
    const auto r = readers.find(key);
    const auto w = writers.find(key);
    return object_ && ((r != readers.end() && r->second) || (w != writers.end() && w->second));
}
Value Model::readSlot(const std::string &, const std::string &) const {
    throw Error("Unbound accessor read");
}
void Model::writeSlot(const std::string &, const std::string &, Value) {
    throw Error("Unbound accessor write");
}
Accessor Description::effectiveAccessor(Limits limits) const {
    if (accessorObject)
        return accessorObject->snapshot(limits);
    if (accessor)
        return *accessor;
    if (!isContainer() && key.empty()) {
        if (!defaultNullAccessor_) {
            Accessor a;
            a.className = "MANullAccessor";
            Value::Array token;
            std::random_device random;
            std::uniform_int_distribution<unsigned> byte(0, 255);
            for (int i = 0; i < 16; ++i)
                token.emplace_back(std::int64_t(byte(random)));
            a.uuid = Value(std::move(token));
            defaultNullAccessor_ = std::move(a);
        }
        return *defaultNullAccessor_;
    }
    Accessor a;
    a.className = isContainer() ? "MAIdentityAccessor" : "MADictionaryAccessor";
    a.name = isContainer() ? "" : key;
    return a;
}
Value Description::readSlot(const std::string &strategy, const std::string &name) const {
    if(strategy == "MASelectorAccessor") {
        need(detail::metadataSelector(*this,name,false), "Unsupported description getter");
        if (name == "defaultLabel")
            return Value(defaultLabel());
        if (name == "isSortable")
            return Value(isSortable());
        if (name == "hasLabel")
            return Value(hasLabel());
        if(name=="type")return Value(detail::descriptionType(*this));
        if(name=="default")return defaultValue();
        if (name == "initialAnswer")
            return initialAnswer();
        auto value=property(name);if(name=="undefined"&&value.isNil())return Value("");return value;
    }
    need(strategy == "MAPropertyAccessor", "Description slot requires property accessor");
    return propertyAt(name, [] { return Value(); });
}
void Description::writeSlot(const std::string &strategy, const std::string &name, Value v) {
    if(strategy == "MASelectorAccessor") {
        need(!name.empty() && name.back()==':', "Description setter requires a keyword selector");
        auto key=name.substr(0,name.size()-1);need(detail::metadataSelector(*this,key,true),"Unsupported description setter");
        if(key=="stringReader"||key=="stringWriter"||key=="validator"){auto cls=std::get_if<Value::Class>(&v.data);need(cls && cls->name==(key=="stringReader"?"MAStringReader":key=="stringWriter"?"MAStringWriter":"MAValidatorVisitor"),"Unsupported interpreter class");}
        if (key == "undefined") {
            setUndefined(std::move(v));
            return;
        }
        if (key == "requiredErrorMessage") {
            setRequiredErrorMessage(std::move(v));
            return;
        }
        if (key == "kindErrorMessage") {
            setKindErrorMessage(std::move(v));
            return;
        }
        if (key == "conflictErrorMessage") {
            setConflictErrorMessage(std::move(v));
            return;
        }
        if (key == "rangeErrorMessage") {
            setRangeErrorMessage(std::move(v));
            return;
        }
        if (key == "initialAnswer") {
            setInitialAnswer(std::move(v));
            return;
        }
        if(key=="default")defaultFunction={};
        properties[key]=std::move(v);return;
    }
    need(strategy == "MAPropertyAccessor", "Description slot requires property accessor");
    (void)propertyPut(name, std::move(v));
}
Value DictionaryModel::readSlot(const std::string &strategy, const std::string &name) const {
    need(strategy == "MADictionaryAccessor", "Dictionary slot requires dictionary accessor");
    auto it = values.find(name);
    return it == values.end() ? Value() : it->second;
}
void DictionaryModel::writeSlot(const std::string &strategy, const std::string &name, Value v) {
    need(strategy == "MADictionaryAccessor", "Dictionary slot requires dictionary accessor");
    values[name] = std::move(v);
}
Value NativeModel::readSlot(const std::string &strategy, const std::string &name) const {
    need(strategy == "MASelectorAccessor" || strategy == "MAVariableAccessor" ||
             strategy == "MAPropertyAccessor",
         "Unsupported native binding strategy");
    auto it = readers.find(strategy + ":" + name);
    need(object_ && it != readers.end(), "Unbound native accessor read");
    return it->second(object_);
}
void NativeModel::writeSlot(const std::string &strategy, const std::string &name, Value v) {
    need(strategy == "MASelectorAccessor" || strategy == "MAVariableAccessor" ||
             strategy == "MAPropertyAccessor",
         "Unsupported native binding strategy");
    auto it = writers.find(strategy + ":" + name);
    need(object_ && it != writers.end(), "Unbound native accessor write");
    it->second(object_, v);
}
Value readString(const Description &d, const std::string &s, Limits l) {
    need(s.size() <= l.bytes && s.size() <= l.visits, "String input byte/work limit");
    (void)Value(s).elements();
    std::size_t work = 0;
    visit(
        d,
        [&](const auto &node) {
            detail::checkDescription(node);
            detail::checkPortableDescription(node, l, work);
        },
        l);
    Work w{l, work};
    return read(d, s, w, 0, true);
}
std::string writeString(const Description &d, const Value &v, const Graph *g, Limits l) {
    std::size_t work = 0;
    visit(
        d,
        [&](const auto &node) {
            detail::checkDescription(node);
            detail::checkPortableDescription(node, l, work);
        },
        l);
    Work w{l, work};
    return write(d, v, g, w, 0, true);
}
std::string writeString(const Description &d, const Model &m, const Graph *g, Limits l) {
    std::size_t work = 0;
    visit(
        d,
        [&](const auto &node) {
            detail::checkDescription(node);
            detail::checkPortableDescription(node, l, work);
        },
        l);
    Work w{l, work};
    return writeModel(d, m, g, w, 0);
}
} // namespace magritte

namespace magritte {
std::string Description::obfuscated(const Value &v, Limits l) const {
    if (!detail::descriptionIs(*this, "MAPasswordDescription"))
        throw Error("Obfuscation requires password description");
    return std::string(Value(writeString(*this, v, nullptr, l)).collectionSize(), '*');
}
bool Description::isObfuscated(const Value &v) const {
    if (!detail::descriptionIs(*this, "MAPasswordDescription"))
        throw Error("Obfuscation requires password description");
    auto s = std::get_if<std::string>(&v.data);
    auto symbol = std::get_if<Value::Symbol>(&v.data);
    if (symbol)
        s = &symbol->utf8;
    return s && !s->empty() && std::all_of(s->begin(), s->end(), [](char c) { return c == '*'; });
}
} // namespace magritte
