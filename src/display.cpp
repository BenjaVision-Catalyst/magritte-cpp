#include "detail.hpp"
#include <algorithm>
#include <limits>
namespace magritte {
void Description::display(std::string selector) {
    displayBlock.reset();
    properties["displayBlockOrSymbol"] = Value(Value::Symbol{std::move(selector)});
}
void Description::display(std::shared_ptr<DisplayBlock> block) {
    displayBlock = std::move(block);
    properties["displayBlockOrSymbol"] = Value();
}
Value Model::displayMessage(const std::string &selector, const Description *argument) const {
    if (argument)
        throw Error("Unbound display message with description argument");
    return readSlot("MASelectorAccessor", selector);
}
Value NativeModel::displayMessage(const std::string &selector, const Description *argument) const {
    auto i = displayReaders.find(selector);
    if (i == displayReaders.end())
        return Model::displayMessage(selector, argument);
    if (!object_)
        throw Error("Null display receiver");
    auto callback = i->second;
    return callback(object_, argument);
}
std::string Description::displayStringFor(const Value &value, const Graph *graph,
                                          Limits limits) const {
    std::size_t work = 0;
    (void)detail::valueKey(value, limits, work);
    // Local validation checks Value shape/UTF-8; the byte limit applies to output text,
    // not the size of a temporary JSON envelope.
    auto validation = limits;
    validation.bytes = std::numeric_limits<std::size_t>::max();
    detail::checkLocalValue(value, validation);
    std::string result;
    if (auto block = displayBlock) {
        if (!block->operation)
            throw Error("Empty display block");
        result = block->operation(value, *this);
    } else {
        auto configured = property("displayBlockOrSymbol");
        auto symbol = std::get_if<Value::Symbol>(&configured.data);
        if (!symbol || symbol->utf8.empty() ||
            !std::all_of(symbol->utf8.begin(), symbol->utf8.end(),
                         [](unsigned char c) { return c >= 33 && c <= 126; }))
            throw Error("Display selector must be nonempty ASCII Symbol");
        const auto &selector = symbol->utf8;
        auto arity = std::count(selector.begin(), selector.end(), ':');
        const std::string binary = "!%&*+,/<=>?@\\~|-";
        if (arity == 0 && std::all_of(selector.begin(), selector.end(),
                                      [&](char c) { return binary.find(c) != std::string::npos; }))
            arity = 1;
        if (arity > 1)
            throw Error("Display selector takes more than one description argument");
        if (std::holds_alternative<Value::Reference>(value.data)) {
            if (!graph)
                throw Error("Display receiver requires graph resolver");
            auto output =
                graph->resolve(value).displayMessage(selector, arity == 1 ? this : nullptr);
            auto text = std::get_if<std::string>(&output.data);
            if (!text)
                throw Error("Display message must return String");
            result = *text;
        } else if (arity == 0 && (selector == "greaseString" || selector == "asString"))
            result = detail::displayString(value);
        else if (selector == "yourself" && std::holds_alternative<std::string>(value.data))
            result = std::get<std::string>(value.data);
        else if (selector == "label" && std::holds_alternative<Value::Class>(value.data))
            result = ClassRegistry::global().find(std::get<Value::Class>(value.data).name)->label;
        else
            throw Error("Unbound scalar display selector: " + selector);
    }
    if (result.size() > limits.bytes)
        throw Error("Display output byte limit");
    (void)detail::valueKey(Value(result), limits, work);
    detail::checkLocalValue(Value(result), validation);
    return result;
}
} // namespace magritte
