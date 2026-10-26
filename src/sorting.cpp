#include "detail.hpp"
#include <algorithm>
#include <atomic>
namespace magritte {
namespace {
std::shared_ptr<AccessorObject> object(Accessor value) {
    static std::atomic<std::uint64_t> serial{0};
    return AccessorObject::fromAccessor("sort.accessor." + std::to_string(++serial), value);
}
Accessor identityAccessor() {
    Accessor a;
    a.className = "MAIdentityAccessor";
    return a;
}
Value send(const Value &left, const std::string &selector, const Value &right, const Graph *graph,
           Limits limits) {
    auto count = std::count(selector.begin(), selector.end(), ':');
    const std::string binary = "!%&*+,/<=>?@\\~|-";
    if (count == 0 && std::all_of(selector.begin(), selector.end(),
                                  [&](char c) { return binary.find(c) != std::string::npos; }))
        count = 1;
    if (count != 1)
        throw Error("Sort message requires one argument");
    detail::checkLocalValue(left, limits);
    detail::checkLocalValue(right, limits);
    if (std::holds_alternative<Value::Reference>(left.data)) {
        if (!graph)
            throw Error("Binary receiver requires graph resolver");
        auto result = graph->resolve(left).binaryMessage(selector, right, graph);
        detail::checkLocalValue(result, limits);
        return result;
    }
    std::size_t work = 0;
    auto leftKey = detail::valueKey(left, limits, work),
         rightKey = detail::valueKey(right, limits, work);
    if (selector == "=" || selector == "~=")
        return Value(selector == "=" ? leftKey == rightKey : leftKey != rightKey);
    if (selector != "<" && selector != "<=" && selector != ">" && selector != ">=")
        throw Error("Unbound scalar binary selector: " + selector);
    int comparison;
    if (detail::isNumber(left) && detail::isNumber(right))
        comparison = detail::compareNumbers(left, right);
    else {
        auto text = [](const Value &v) -> std::optional<std::string> {
            if (auto s = std::get_if<std::string>(&v.data))
                return *s;
            if (auto s = std::get_if<Value::Symbol>(&v.data))
                return s->utf8;
            return {};
        };
        std::string a, b;
        auto x = text(left), y = text(right);
        if (x && y) {
            a = *x;
            b = *y;
        } else if (left.data.index() == 6 && right.data.index() == 6) {
            a = std::get<Value::Character>(left.data).utf8;
            b = std::get<Value::Character>(right.data).utf8;
        } else
            throw Error("Unsupported ordered receiver/argument");
        comparison = a < b ? -1 : a > b ? 1 : 0;
    }
    return Value(selector == "<"    ? comparison < 0
                 : selector == "<=" ? comparison <= 0
                 : selector == ">"  ? comparison > 0
                                    : comparison >= 0);
}
} // namespace
SortBlock::SortBlock(std::string selector) : SortBlock(identityAccessor(), std::move(selector)) {}
SortBlock::SortBlock(Accessor accessor, std::string selector)
    : SortBlock(object(std::move(accessor)), std::move(selector)) {}
SortBlock::SortBlock(std::shared_ptr<AccessorObject> accessor, std::string selector)
    : accessor_(std::move(accessor)), selector_(std::move(selector)) {
    if (!accessor_ || selector_.empty() ||
        !std::all_of(selector_.begin(), selector_.end(),
                     [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Error("Invalid sort block initialization");
}
Value SortBlock::value(const Value &first, const Value &second, const Graph *graph,
                       Limits limits) const {
    auto left = accessor_->snapshot(limits).readValue(first, graph, limits);
    auto message = selector_;
    auto right = accessor_->snapshot(limits).readValue(second, graph, limits);
    return send(left, message, right, graph, limits);
}
Value SortBlock::value(const Model &first, const Model &second, const Graph *graph,
                       Limits limits) const {
    auto left = accessor_->snapshot(limits).read(first, graph, limits);
    auto message = selector_;
    auto right = accessor_->snapshot(limits).read(second, graph, limits);
    return send(left, message, right, graph, limits);
}
Value Model::binaryMessage(const std::string &selector, const Value &argument,
                           const Graph *graph) const {
    if (selector == "~=") {
        auto equal = binaryMessage("=", argument, graph);
        auto boolean = std::get_if<bool>(&equal.data);
        if (!boolean)
            throw Error("Equality result does not understand not");
        return Value(!*boolean);
    }
    if (selector == "=") {
        bool equal = false;
        if (std::holds_alternative<Value::Reference>(argument.data)) {
            if (!graph)
                throw Error("Binary argument requires graph resolver");
            equal = &graph->resolve(argument) == this;
        }
        return Value(equal);
    }
    throw Error("Unbound binary message: " + selector);
}
Value NativeModel::binaryMessage(const std::string &selector, const Value &argument,
                                 const Graph *graph) const {
    if (!object_)
        throw Error("Null binary receiver");
    auto found = binaryReaders.find(selector);
    if (found != binaryReaders.end()) {
        auto callback = found->second;
        return callback(object_, argument, graph);
    }
    if (selector == "=") {
        bool equal = false;
        if (std::holds_alternative<Value::Reference>(argument.data)) {
            if (!graph)
                throw Error("Binary argument requires graph resolver");
            auto other = dynamic_cast<const NativeModel *>(&graph->resolve(argument));
            equal = other && object_ == other->object_;
        }
        return Value(equal);
    }
    return Model::binaryMessage(selector, argument, graph);
}
bool Description::isSortable() const {
    auto &registry = ClassRegistry::global();
    if (registry.includesBehavior(className, "MAPasswordDescription"))
        return false;
    return registry.includesBehavior(className, "MAStringDescription") ||
           registry.includesBehavior(className, "MAMagnitudeDescription");
}
} // namespace magritte
