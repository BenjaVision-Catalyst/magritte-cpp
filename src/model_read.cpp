#include "detail.hpp"
namespace magritte {
namespace detail {
Value undefinedIfNil(const Description &description, Value value) {
    return value.isNil() ? description.property("undefinedValue") : std::move(value);
}
} // namespace detail
Value Model::readUsing(const Description &description) const {
    auto value = description.effectiveAccessor().read(*this);
    return detail::undefinedIfNil(description, std::move(value));
}
} // namespace magritte
