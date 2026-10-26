#include "detail.hpp"
#include <mutex>
#include <algorithm>
namespace magritte {
namespace {
std::mutex registryMutex;
std::map<std::string, DescriptionBuilderFactory> &factories() {
    static std::map<std::string, DescriptionBuilderFactory> result{
        {"MADescriptionBuilder", [](Limits l) { return std::make_unique<DescriptionBuilder>(l); }},
        {"MAComposedDescriptionBuilder",
         [](Limits l) { return std::make_unique<ComposedDescriptionBuilder>(l); }},
        {"MADescriptionFlattener",
         [](Limits l) { return std::make_unique<DescriptionFlattener>(l); }}};
    return result;
}
std::shared_ptr<Description> describe(const Model *target, const BuilderEnvironment &env) {
    if (env.describe)
        return env.describe(target);
    if (auto a = dynamic_cast<const AdaptiveModel *>(target))
        return a->description;
    if (!target)
        return {};
    return {}; // undescribed native objects have no annotated fields by default
}
} // namespace
void registerDescriptionBuilder(const std::string &name, DescriptionBuilderFactory factory) {
    if (!factory)
        throw Error("Empty builder factory");
    auto info = ClassRegistry::global().find(name);
    if (!info)
        ClassRegistry::global().registerClass({name, "MADescriptionBuilder", false, name});
    else if (!ClassRegistry::global().includesBehavior(name, "MADescriptionBuilder"))
        throw Error("Class is not a description builder");
    std::lock_guard<std::mutex> lock(registryMutex);
    if (!factories().emplace(name, std::move(factory)).second)
        throw Error("Builder factory already registered");
}
BuilderStep BuilderStep::named(const std::string &name) {
    return {[name](std::shared_ptr<Description> source, const std::string &prefix,
                   DescriptionBuilder &context) {
        DescriptionBuilderFactory factory;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto found = factories().find(name);
            if (found == factories().end())
                throw Error("No buildFrom:target:model: implementation: " + name);
            factory = found->second;
        }
        if (!context.environment.budget)
            context.environment.budget = std::make_shared<BuilderBudget>();
        auto budget = context.environment.budget;
        if (!budget->depth)
            budget->work = 0;
        if (budget->depth >= context.builderLimits().depth ||
            ++budget->work > context.builderLimits().visits)
            throw Error("Builder pipeline depth/work limit");
        ++budget->depth;
        struct Leave {
            std::shared_ptr<BuilderBudget> b;
            ~Leave() { --b->depth; }
        } leave{budget};
        auto builder = factory(context.builderLimits());
        if (!builder)
            throw Error("Builder factory returned null");
        builder->environment = context.environment;
        builder->model = context.model;
        return context.targetValue ? builder->buildFromValue(source, prefix, *context.targetValue)
                                   : builder->buildFrom(source, prefix, context.target);
    }};
}
bool canExpand(const Description &d, const BuilderEnvironment &env) {
    auto explicitValue = d.properties.find("canExpand");
    if (explicitValue != d.properties.end()) {
        if (auto flag = std::get_if<bool>(&explicitValue->second.data))
            return *flag;
        throw Error("canExpand must be Boolean");
    }
    if (!d.flag("visible") || d.flag("readOnly"))
        return false;
    if (!env.tabDisplay)
        throw Error("Message not understood: tabDisplay");
    if (env.tabDisplay(d))
        return false;
    return d.property("group").isNil();
}
void setDescriptionBuilders(Description &d, const std::vector<std::string> &names, Limits limits) {
    if (!d.isContainer() || names.size() > limits.items)
        throw Error("Invalid descriptionBuilders receiver/items");
    Value::Array values;
    for (const auto &name : names)
        values.push_back(Value(Value::Class{name}));
    detail::checkLocalValue(Value(values), limits);
    d.properties["descriptionBuilder"] = Value(Value::Class{"MAComposedDescriptionBuilder"});
    d.properties["descriptionBuilders"] = Value(values);
}
Model *DescriptionBuilder::targetModel() const {
    if (!targetValue)
        return target;
    if (targetValue->isNil())
        return nullptr;
    if (!environment.graph)
        throw Error("Builder target requires graph resolver");
    return &environment.graph->resolve(*targetValue);
}
std::shared_ptr<Description> DescriptionBuilder::targetDescription(const Model *target) const {
    return describe(target, environment);
}
std::shared_ptr<Description> DescriptionBuilder::currentTargetDescription() const {
    if (targetValue && targetValue->data.index() != 5)
        return {};
    return targetDescription(targetModel());
}
std::vector<BuilderStep> DescriptionBuilder::stepsFor(const Description &d) const {
    if (environment.descriptionBuilders)
        return environment.descriptionBuilders(d);
    auto values = d.property("descriptionBuilders");
    std::vector<BuilderStep> result;
    for (const auto &v : values.elements()) {
        auto c = std::get_if<Value::Class>(&v.data);
        if (!c)
            throw Error("Builder entry is not an executable Class");
        result.push_back(BuilderStep::named(c->name));
    }
    return result;
}
DescriptionCopy DescriptionBuilder::buildFromValue(std::shared_ptr<Description> source,
                                                   const std::string &prefix, Value value) {
    detail::checkLocalValue(value, limits_);
    auto saved =
        value; // preserve collection-valued targets rather than resolving them as single models
    // buildFrom initializes ownership and identity namespace, but must not visit twice.
    if (prefix.empty() || !std::all_of(prefix.begin(), prefix.end(),
                                       [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Error("Invalid builder identity prefix");
    remember(source);
    prefix_ = prefix;
    target = nullptr;
    targetValue = std::move(saved);
    visit(*source);
    return result_;
}
void ComposedDescriptionBuilder::compose(std::shared_ptr<Description> source,
                                         const std::vector<BuilderStep> &steps, Model *nextTarget,
                                         std::optional<Value> value) {
    if (steps.size() > limits_.items)
        throw Error("Builder pipeline item limit");
    DescriptionBuilder::setBuiltDescription(source);
    auto oldTarget = target;
    auto oldValue = targetValue;
    target = nextTarget;
    targetValue = std::move(value);
    try {
        for (const auto &step : steps) {
            if (++pipelineSerial_ > limits_.visits || !step.operation)
                throw Error("Builder pipeline work/missing operation");
            auto next = step.operation(
                result_.root, prefix_ + ".pipeline." + std::to_string(pipelineSerial_), *this);
            for (const auto &owner : next.retained)
                remember(owner);
            DescriptionBuilder::setBuiltDescription(next.root);
        }
    } catch (...) {
        target = oldTarget;
        targetValue = oldValue;
        throw;
    }
    target = oldTarget;
    targetValue = oldValue;
}
void ComposedDescriptionBuilder::setBuiltDescription(std::shared_ptr<Description> source) {
    auto schema = currentTargetDescription();
    auto steps = schema ? stepsFor(*schema) : std::vector<BuilderStep>{};
    compose(source, steps, target, targetValue);
}
void ComposedDescriptionBuilder::visitContainer(Description &d) {
    compose(ownedDescription(d), stepsFor(d), target, targetValue);
}
void ComposedDescriptionBuilder::visitRelationDescription(Description &d) {
    auto actual = targetModel();
    auto schema = targetDescription(actual);
    auto steps = schema ? stepsFor(*schema) : std::vector<BuilderStep>{};
    if (!actual)
        throw Error("Nil target readUsing: protocol");
    auto value = actual->readUsing(d);
    if (!value.isNil()) {
        auto saved = result_.root;
        try {
            compose(d.referenceDescription(), steps, nullptr, value);
            auto reference = result_.root;
            result_.root = saved;
            d.reference = reference;
        } catch (...) {
            result_.root = saved;
            throw;
        }
    }
    DescriptionBuilder::visitRelationDescription(d);
}
void DescriptionFlattener::visitToOneRelationDescription(Description &d) {
    if (!canExpand(d, environment)) {
        DescriptionBuilder::visitToOneRelationDescription(d);
        return;
    }
    Model *sample = target;
    if (targetValue) {
        auto v = *targetValue;
        if (v.isCollection()) {
            if (!v.collectionSize()) {
                DescriptionBuilder::visitToOneRelationDescription(d);
                return;
            }
            v = v.elements().front();
        }
        if (v.isNil())
            sample = nullptr;
        else {
            if (!environment.graph)
                throw Error("Flattener sample requires graph resolver");
            sample = &environment.graph->resolve(v);
        }
    }
    if (!sample)
        throw Error("Nil sample readUsing: protocol");
    auto value = sample->readUsing(d);
    if (value.isNil())
        DescriptionBuilder::visitToOneRelationDescription(d);
    else
        expandDescription(d, value);
}
void DescriptionFlattener::expandDescription(Description &parent, const Value &value) {
    if (!expansionDepth_)
        expansionWork_ = 0;
    if (expansionDepth_ >= limits_.depth)
        throw Error("Flattener depth limit");
    ++expansionDepth_;
    try {
        Model *model = nullptr;
        if (!value.isNil() && value.data.index() != 5) {
            --expansionDepth_;
            return;
        }
        if (!value.isNil()) {
            if (!environment.graph)
                throw Error("Flattener expansion requires graph resolver");
            model = &environment.graph->resolve(value);
        }
        auto schema = targetDescription(model);
        if (schema) {
            remember(schema);
            auto children = schema->orderedChildren();
            if (children.size() > limits_.items)
                throw Error("Flattener item limit");
            for (const auto &child : children) {
                if (++expansionWork_ > limits_.visits)
                    throw Error("Flattener work limit");
                if (!model)
                    throw Error("Nil expansion readUsing: protocol");
                auto next = model->readUsing(*child); // read before mutating the shared description
                if (!environment.addToAccessor)
                    throw Error("Message not understood: addToAccessor:");
                environment.addToAccessor(*child, parent.effectiveAccessor(limits_));
                child->properties["priority"] =
                    detail::flattenedPriority(parent.priorityValue(), child->priorityValue());
                if (!environment.groupExpandOnLabel)
                    throw Error("Message not understood: groupExpandOnLabel");
                child->properties["group"] =
                    parent.property(environment.groupExpandOnLabel(parent) ? "label" : "group");
                addToBuiltDescription(*child, next);
            }
        }
    } catch (...) {
        --expansionDepth_;
        throw;
    }
    --expansionDepth_;
}
void DescriptionFlattener::addToBuiltDescription(Description &d, const Value &value) {
    if (canExpand(d, environment))
        expandDescription(d, value);
    else {
        if (!result_.root || !result_.root->isContainer())
            throw Error("Built description cannot add field");
        if (result_.root->children.size() >= limits_.items)
            throw Error("Flattener item limit");
        result_.root->children.push_back(ownedDescription(d));
    }
}
} // namespace magritte
