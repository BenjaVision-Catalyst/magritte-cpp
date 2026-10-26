#include "detail.hpp"
#include <limits>
namespace magritte {
void PrintStream::append(const std::string &text) {
    if (contents_.size() > limits_.bytes || text.size() > limits_.bytes - contents_.size())
        throw Error("Print output byte limit");
    if (work_ >= limits_.visits || text.size() >= limits_.visits - work_)
        throw Error("Print output work limit");
    auto validation = limits_;
    validation.bytes = std::numeric_limits<std::size_t>::max();
    detail::checkLocalValue(Value(text), validation);
    const auto size = text.size();
    contents_ += text;
    work_ += size + 1;
}
Description &Description::setPrintString(std::shared_ptr<PrintValueBlock> block, Limits limits) {
    (void)propertyPut("maPrintString", Value(), limits);
    printStringBlock = std::move(block);
    if (!printStringBlock)
        properties.erase("maPrintString");
    return *this;
}
Description &Description::setPrintSeparator(std::shared_ptr<PrintSeparatorBlock> block,
                                            Limits limits) {
    if (!isContainer())
        throw Error("Print separator requires a container");
    (void)propertyPut("maPrintStringSeparator", Value(), limits);
    printSeparatorBlock = std::move(block);
    if (!printSeparatorBlock)
        properties.erase("maPrintStringSeparator");
    return *this;
}
std::string Description::readToString(const Model &model, const Graph *graph, Limits limits) const {
    auto value = model.readUsing(*this);
    return writeString(*this, value, graph, limits);
}
void Description::printFor(const Model &model, PrintStream &stream, const Graph *graph) const {
    if (!ClassRegistry::global().includesBehavior(className, "MAElementDescription"))
        throw Error("printFor requires an element description");
    auto value = model.readUsing(*this);
    if (!value.isNil())
        stream.append(writeString(*this, value, graph, stream.limits()));
}
void Description::print(const Model &model, PrintStream &stream, const Graph *graph) const {
    if (!isContainer())
        throw Error("print requires a container");
    // Upstream captures the separator once, before any field reads.
    auto separator = printSeparatorBlock;
    bool hasSeparator = hasProperty("maPrintStringSeparator");
    auto fields = enumerationChildren(*this, stream.limits());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) {
            if (separator) {
                if (!separator->operation)
                    throw Error("Empty print separator");
                separator->operation(stream);
            } else if (hasSeparator)
                throw Error("Print separator has no local callable representation");
            else
                stream.append(" ");
        }
        auto field = fields[i];
        auto value = model.readUsing(*field);
        if (value.isNil())
            continue;
        std::size_t work = 0;
        (void)detail::valueKey(value, stream.limits(), work);
        auto validation = stream.limits();
        validation.bytes = std::numeric_limits<std::size_t>::max();
        detail::checkLocalValue(value, validation);
        if (auto block = field->printStringBlock) {
            if (!block->operation)
                throw Error("Empty print formatter");
            stream.append(block->operation(value));
        } else if (field->hasProperty("maPrintString"))
            throw Error("Print formatter has no local callable representation");
        else
            stream.append(writeString(*field, value, graph, stream.limits()));
    }
}
} // namespace magritte
