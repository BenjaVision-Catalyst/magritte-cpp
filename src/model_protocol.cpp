#include "detail.hpp"
namespace magritte {
namespace {
struct Work {
    Limits limits;
    std::size_t count = 0;
    void tick() {
        if (++count > limits.visits)
            throw Error("Model protocol work limit");
    }
    void value(const Value &v) {
        detail::checkRuntimeValue(v, limits, count);
        detail::checkLocalValue(v, limits);
    }
    bool equal(const Value &left, const Value &right, const Graph *graph) {
        tick();
        if (std::holds_alternative<Value::Reference>(left.data)) {
            if (!graph)
                throw Error("Model equality receiver requires a graph resolver");
            auto result = graph->resolve(left).binaryMessage("=", right, graph);
            value(result);
            auto boolean = std::get_if<bool>(&result.data);
            if (!boolean)
                throw Error("Model equality message must return Boolean");
            return *boolean;
        }
        return detail::valuesEqual(left,right,limits,count);
    }
};
Value declaredDefault(const Description &d, Work &work) {
    if (d.defaultFunction)
        throw Error("Model initialization/comparison does not evaluate callable defaults");
    auto value = d.property(d.isContainer() ? "undefinedValue" : "default", work.limits);
    work.value(value);
    return value;
}
std::shared_ptr<Description>
resolve(const std::function<std::shared_ptr<Description>()> &provider) {
    if (!provider)
        throw Error("Missing model description provider");
    auto d = provider();
    if (!d)
        throw Error("Model description provider returned null");
    return d;
}
} // namespace
Model &initializeMagritte(Model &model, const Description &description, Limits limits) {
    Work work{limits};
    for (const auto &d : enumerationChildren(description, limits)) {
        work.tick();
        if (!d->flag("readOnly"))
            model.writeUsing(*d, declaredDefault(*d, work));
    }
    return model;
}
Model &initializeMagritte(Model &model,
                          const std::function<std::shared_ptr<Description>()> &provider,
                          Limits limits) {
    auto d = resolve(provider);
    return initializeMagritte(model, *d, limits);
}
bool isSameAs(const Model &model, const Model &other, const Description &description,
              const Graph *graph, Limits limits) {
    Work work{limits};
    for (const auto &d : enumerationChildren(description, limits)) {
        work.tick();
        if (!d->effectiveAccessor(limits).canRead(other, graph, limits))
            return false;
        auto left = model.readUsing(*d);
        auto right = other.readUsing(*d);
        auto fallback = declaredDefault(*d, work);
        work.value(left);
        work.value(right);
        auto matchesDefault = [&](const Value &candidate) {
            return work.equal(Value(), candidate, graph) || work.equal(fallback, candidate, graph);
        };
        if (matchesDefault(left) && matchesDefault(right))
            continue;
        if (!work.equal(left, right, graph))
            return false;
    }
    return true;
}
bool isSameAs(const Model &model, const Model &other,
              const std::function<std::shared_ptr<Description>()> &provider, const Graph *graph,
              Limits limits) {
    auto d = resolve(provider);
    return isSameAs(model, other, *d, graph, limits);
}
} // namespace magritte
