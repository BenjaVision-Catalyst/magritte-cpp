#include "detail.hpp"
#include <cmath>
#include <limits>
#include <numeric>
namespace magritte {
namespace {
std::uint64_t magnitude(std::int64_t n) {
    return n < 0 ? std::uint64_t(-(n + 1)) + 1 : std::uint64_t(n);
}
Value::Fraction normalized(Value::Fraction f) {
    if (f.denominator <= 0)
        throw Error("Fraction denominator must be positive");
    auto g = std::gcd(magnitude(f.numerator), std::uint64_t(f.denominator));
    if (g) {
        f.numerator /= std::int64_t(g);
        f.denominator /= std::int64_t(g);
    }
    return f;
}
std::int64_t mul(std::int64_t a, std::int64_t b) {
    if (b <= 0)
        throw Error("Invalid priority multiplier");
    if (a > std::numeric_limits<std::int64_t>::max() / b ||
        a < std::numeric_limits<std::int64_t>::min() / b)
        throw Error("Fraction priority overflow");
    return a * b;
}
int unsignedCompare(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    int direction = 1;
    while (true) {
        auto x = a / b, y = c / d;
        if (x != y)
            return (x < y ? -1 : 1) * direction;
        auto r = a % b, s = c % d;
        if (!r || !s)
            return (r == s ? 0 : !r ? -1 : 1) * direction;
        a = b;
        b = r;
        c = d;
        d = s;
        direction = -direction;
    }
}
int fractionFloat(Value::Fraction x, double y) {
    if (!std::isfinite(y))
        throw Error("Non-finite float");
    if (x.numerator < 0 && y >= 0)
        return -1;
    if (x.numerator >= 0 && y < 0)
        return 1;
    bool negative = x.numerator < 0;
    auto n = magnitude(x.numerator), d = std::uint64_t(x.denominator);
    double f = std::fabs(y);
    if (f >= 18446744073709551616.0)
        return negative ? 1 : -1;
    auto integer = std::uint64_t(f), q = n / d;
    int result;
    if (q != integer)
        result = q < integer ? -1 : 1;
    else {
        auto r = n % d;
        f -= double(integer);
        result = 0;
        while (r || f != 0) {
            r *= 2;
            f *= 2;
            auto bit = r >= d;
            bool other = f >= 1;
            if (bit != other) {
                result = bit ? 1 : -1;
                break;
            }
            if (bit)
                r -= d;
            if (other)
                f -= 1;
        }
    }
    return negative ? -result : result;
}
} // namespace
Value::Value(Fraction f) {
    f = normalized(f);
    if (f.denominator == 1)
        data = f.numerator;
    else
        data = f;
}
namespace detail {
std::optional<Value::Fraction> floatFraction(double n) {
    if (!std::isfinite(n))
        throw Error("Non-finite float");
    if (n == 0)
        return Value::Fraction{0, 1};
    int exp;
    auto f = std::frexp(std::fabs(n), &exp);
    auto bits = std::uint64_t(std::ldexp(f, 53));
    exp -= 53;
    while (!(bits & 1)) {
        bits >>= 1;
        ++exp;
    }
    if (exp < 0) {
        if (-exp > 62 || bits > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            return {};
        return Value::Fraction{n < 0 ? -std::int64_t(bits) : std::int64_t(bits), std::int64_t(1)
                                                                                     << -exp};
    }
    if (exp > 62 || bits > (std::uint64_t(std::numeric_limits<std::int64_t>::max()) >> exp))
        return {};
    return Value::Fraction{n < 0 ? -std::int64_t(bits << exp) : std::int64_t(bits << exp), 1};
}
Value::Fraction fractionOf(const Value &v) {
    if (auto i = std::get_if<std::int64_t>(&v.data))
        return {*i, 1};
    if (auto f = std::get_if<Value::Fraction>(&v.data))
        return normalized(*f);
    if (auto f = std::get_if<double>(&v.data)) {
        auto r = floatFraction(*f);
        if (r)
            return *r;
    }
    throw Error("Priority cannot be represented as bounded exact fraction");
}
int compareFractions(const Value &a, const Value &b) {
    if (a.data.index() == 9)
        return -fractionFloat(fractionOf(b), std::get<double>(a.data));
    if (b.data.index() == 9)
        return fractionFloat(fractionOf(a), std::get<double>(b.data));
    auto x = fractionOf(a), y = fractionOf(b);
    if (x.numerator < 0 && y.numerator >= 0)
        return -1;
    if (x.numerator >= 0 && y.numerator < 0)
        return 1;
    auto r = unsignedCompare(magnitude(x.numerator), x.denominator, magnitude(y.numerator),
                             y.denominator);
    return x.numerator < 0 ? -r : r;
}
Value flattenedPriority(const Value &parent, const Value &child) {
    auto a = fractionOf(parent), b = fractionOf(child);
    auto g = std::gcd(magnitude(b.numerator), std::uint64_t(1000));
    b.numerator /= std::int64_t(g);
    b.denominator = mul(b.denominator, 1000 / std::int64_t(g));
    auto common = std::gcd(a.denominator, b.denominator);
    auto left = mul(a.numerator, b.denominator / common),
         right = mul(b.numerator, a.denominator / common);
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
        throw Error("Fraction priority overflow");
    return Value(Value::Fraction{left + right, mul(a.denominator, b.denominator / common)});
}
} // namespace detail
Value Description::priorityValue() const {
    auto p = property("priority");
    if (!detail::isNumber(p))
        throw Error("Expected numeric priority");
    return p;
}
} // namespace magritte
