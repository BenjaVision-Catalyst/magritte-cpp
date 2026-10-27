#include "detail.hpp"
#include <set>
namespace magritte {
ValidationIssue::ValidationIssue(
    const Description &d, std::string cls, std::optional<std::string> text,
    std::vector<std::shared_ptr<ValidationIssue>> errors)
    : owner_(d.weak_from_this().lock()), tag(&d), code(std::move(cls)),
      signalText(std::move(text)), collection(std::move(errors)) {
  if (!kindOf("MAValidationError"))
    throw Error("Validation issue class must inherit MAValidationError");
  if (!kindOf("MAMultipleErrors") && !collection.empty())
    throw Error("Only multiple errors have a collection");
  for (const auto &e : collection)
    if (!e)
      throw Error("Null nested validation issue");
}
bool ValidationIssue::kindOf(const std::string &cls) const {
  return ClassRegistry::global().includesBehavior(code, cls);
}
std::string ValidationIssue::printString(Limits limits) const {
  std::size_t work = 0;
  std::string output;
  auto append = [&](const std::string &s) {
    if (s.size() > limits.bytes - output.size())
      throw Error("Validation print byte limit");
    output += s;
  };
  std::function<void(const ValidationIssue &, std::size_t)> walk =
      [&](const auto &e, auto depth) {
        if (++work > limits.visits || depth > limits.depth ||
            e.collection.size() > limits.items)
          throw Error("Validation print work/depth/item limit");
        if (e.kindOf("MAMultipleErrors")) {
          bool first = true;
          for (auto &child : e.collection) {
            if (!first)
              append("\r");
            first = false;
            walk(*child, depth + 1);
          }
        } else {
          auto label = e.tag->property("label", limits);
          if (!label.isNil()) {
            append(detail::displayString(label));
            append(": ");
          }
          append(e.messageText());
        }
      };
  walk(*this, 0);
  return output;
}
std::vector<ValidationError> ValidationResult::flatErrors(Limits limits) const {
  std::vector<ValidationError> out;
  std::size_t work = 0;
  std::function<void(const ValidationIssue &, std::size_t)> walk =
      [&](const auto &e, auto depth) {
        if (++work > limits.visits || depth > limits.depth ||
            e.collection.size() > limits.items)
          throw Error("Validation projection limit");
        if (e.kindOf("MAMultipleErrors") && !e.collection.empty())
          for (auto &child : e.collection)
            walk(*child, depth + 1);
        else {
          if (out.size() >= limits.items)
            throw Error("Validation leaf item limit");
          out.push_back({e.tag->id, e.code, e.messageText()});
        }
      };
  if (error)
    walk(*error, 0);
  return out;
}
ValidationFailure::ValidationFailure(std::shared_ptr<ValidationIssue> e)
    : Error(e ? e->messageText() : "Missing validation error"),
      error(std::move(e)) {
  if (!error)
    throw Error("Missing validation error");
}
void ValidationResult::raiseIfInvalid() const {
  if (error)
    throw ValidationFailure(error);
}
ValidationContext::ValidationContext(const Description &d, Limits l)
    : description_(d), limits_(l) {}
std::shared_ptr<ValidationIssue>
ValidationContext::signal(std::string code, std::optional<std::string> text) {
  if (errors_.size() >= limits_.items || errors_.size() >= limits_.visits)
    throw Error("Validation error item/work limit");
  if (text) {
    if (text->size() > limits_.bytes)
      throw Error("Validation message byte limit");
    auto l = limits_;
    l.bytes = static_cast<std::size_t>(-1);
    detail::checkLocalValue(Value(*text), l);
  }
  auto error = std::make_shared<ValidationIssue>(description_, std::move(code),
                                                 std::move(text));
  errors_.push_back(error);
  return error;
}
void ValidationContext::include(const ValidationResult &r) {
  if (!r.error)
    return;
  if (errors_.size() >= limits_.items || errors_.size() >= limits_.visits)
    throw Error("Validation error item/work limit");
  errors_.push_back(r.error);
}
void ValidationContext::tryValidation(const Block &body, const Block &ifPass) {
  if (!body || !ifPass)
    throw Error("Missing validation block");
  auto before = errors_.size();
  body(*this);
  if (errors_.size() == before)
    ifPass(*this);
}
ValidationResult ValidationContext::finish() const {
  if (errors_.empty())
    return {};
  auto label = description_.property("label", limits_);
  std::optional<std::string> text;
  if (!label.isNil())
    text = detail::displayString(label);
  return {std::make_shared<ValidationIssue>(description_, "MAMultipleErrors",
                                            text, errors_)};
}
namespace {
struct Target {
  const Value *value = nullptr;
  const Model *model = nullptr;
};
struct Validator {
  const Graph *graph;
  Limits limits;
  std::size_t work = 0;
  std::set<const void *> collections;
  void tick(std::size_t depth) {
    if (++work > limits.visits || depth > limits.depth)
      throw Error("Validation work/depth limit");
  }
  const Model *model(Target t) const {
    if (t.model)
      return t.model;
    if (t.value && std::holds_alternative<Value::Reference>(t.value->data)) {
      if (!graph)
        throw Error("Object reference validation requires a graph resolver");
      return &graph->resolve(*t.value);
    }
    return nullptr;
  }
  bool boolean(Value v) {
    detail::checkRuntimeValue(v, limits, work);
    auto b = std::get_if<bool>(&v.data);
    if (!b)
      throw Error("Validation message must return Boolean");
    return *b;
  }
  bool kind(Target t, const std::string &cls) {
    if (auto m = model(t))
      return m->kindOf(cls);
    auto &v = *t.value;
    if (auto c = v.conflict())
      return c->kindOf(cls);
    std::string type;
    if (v.isMutableCollection()) {
      auto k = v.mutableCollection()->kind();
      type = k == "array"     ? "Array"
             : k == "ordered" ? "OrderedCollection"
             : k == "set"     ? "Set"
             : k == "bag"     ? "Bag"
                              : "String";
    } else {
      static const char *types[] = {
          "UndefinedObject", "Boolean", "Integer",   "String",
          "Array",           nullptr,   "Character", nullptr,
          "Symbol",          "Float",   "Class",     "MACompositeConflict",
          "Fraction"};
      type = types[v.data.index()] ? types[v.data.index()] : "";
      if (auto c = std::get_if<Value::Collection>(&v.data))
        type = c->kind == "ordered" ? "OrderedCollection"
               : c->kind == "set"   ? "Set"
                                    : "Bag";
    }
    return !type.empty() && ClassRegistry::global().includesBehavior(type, cls);
  }
  void references(const Value &v, std::size_t depth) {
    tick(depth);
    if (v.isMutableCollection()) {
      if (!collections.insert(v.mutableCollection()->identity()).second)
        return;
      references(v.mutableCollection()->snapshot(), depth);
    } else if (std::holds_alternative<Value::Reference>(v.data)) {
      (void)model({&v});
    } else if (v.data.index() == 4 || v.data.index() == 7) {
      if (v.collectionSize() > limits.items)
        throw Error("Validation collection item limit");
      for (const auto &e : v.elements())
        references(e, depth + 1);
    }
  }
  bool undefined(Target t, const Description &d) {
    auto sentinel = d.property("undefinedValue", limits);
    if (auto m = model(t))
      return boolean(m->binaryMessage("=", sentinel, graph));
    return detail::valuesEqual(*t.value, sentinel, limits, work);
  }
  void emit(ValidationContext &c, const Description &d, const std::string &code,
            std::size_t depth) {
    tick(depth);
    c.signal(code, detail::validationMessage(d, code, limits));
  }
  void kindStage(const Description &d, Target t, ValidationContext &c,
                 std::size_t depth) {
    auto expected = d.property("kind", limits);
    auto cls = std::get_if<Value::Class>(&expected.data);
    if (!cls)
      throw Error("Description kind is nil");
    auto before = c.size();
    if (!kind(t, cls->name))
      emit(c, d, "MAKindError", depth);
    bool multipleRelation =
             detail::descriptionIs(d, "MAToManyRelationDescription"),
         multipleOption =
             detail::descriptionIs(d, "MAMultipleOptionDescription");
    if ((multipleRelation || multipleOption) && c.size() != before)
      return;
    auto relation = [&](const Value &v) {
      bool accepted = false;
      for (const auto &cls : d.classes) {
        tick(depth);
        if (kind({&v}, cls))
          accepted = true;
      }
      if (!accepted)
        emit(c, d, "MAKindError", depth);
    };
    auto option = [&](const Value &v) {
      bool member = false;
      if (d.options.isDynamic())
        member = d.options.provider()->includes(v, limits);
      else
        for (const auto &o : d.options.values(limits)) {
          tick(depth);
          if (detail::valuesEqual(v, o, limits, work)) {
            member = true;
            break;
          }
        }
      if (member)
        return;
      if (!d.flag("extensible"))
        emit(c, d, "MAKindError", depth);
      else
        c.include(run(*d.referenceDescription(), {&v}, depth + 1));
    };
    if (d.isRelation()) {
      if (multipleRelation) {
        if (!t.value)
          throw Error("Native model collection enumeration requires an "
                      "explicit Value adapter");
        for (const auto &e : t.value->elements()) {
          tick(depth);
          relation(e);
        }
      } else if (t.value)
        relation(*t.value);
      else {
        bool accepted = false;
        for (const auto &cls : d.classes) {
          tick(depth);
          if (kind(t, cls))
            accepted = true;
        }
        if (!accepted)
          emit(c, d, "MAKindError", depth);
      }
    } else if (d.isOption()) {
      if (!t.value)
        throw Error("Native model options require an explicit Value adapter");
      if (multipleOption)
        for (const auto &e : t.value->elements()) {
          tick(depth);
          option(e);
        }
      else
        option(*t.value);
    }
  }
  void own(const Description &d, Target t, ValidationContext &c,
           std::size_t depth) {
    bool nil = t.value && t.value->isNil();
    if (d.flag("required") && nil)
      emit(c, d, "MARequiredError", depth);
    if (d.flag("required") && d.isMultiple() && t.value &&
        t.value->isCollection() && t.value->collectionSize() == 0)
      emit(c, d, "MARequiredError", depth);
    if (undefined(t, d))
      return;
    c.tryValidation(
        [&](auto &stage) { kindStage(d, t, stage, depth); },
        [&](auto &stage) {
          if (detail::descriptionIs(d, "MANumberDescription")) {
            if (!t.value)
              throw Error(
                  "Native magnitude requires an explicit Value adapter");
            auto min = d.property("min"), max = d.property("max");
            if ((!min.isNil() && detail::compareNumbers(*t.value, min) < 0) ||
                (!max.isNil() && detail::compareNumbers(*t.value, max) > 0))
              emit(stage, d, "MARangeError", depth);
          }
          for (const auto &condition : d.portableConditions) {
            tick(depth);
            bool pass;
            if (auto m = model(t))
              pass = detail::evaluateModelPredicate(condition.predicate, *m,
                                                    graph, limits, work);
            else
              pass = detail::evaluatePredicate(condition.predicate, *t.value,
                                               limits, work);
            if (!pass)
              stage.signal("MAConditionError", condition.label);
          }
          for (const auto &condition : d.conditions) {
            tick(depth);
            bool pass;
            if (condition.modelTest) {
              auto m = model(t);
              if (!m)
                throw Error("Model condition requires an actual model target");
              pass = condition.modelTest(*m);
            } else {
              if (!condition.test)
                throw Error("Missing local condition function");
              if (!t.value)
                throw Error(
                    "Native model condition requires a modelTest binding");
              pass = condition.test(*t.value);
            }
            if (!pass)
              stage.signal("MAConditionError", condition.label);
          }
        });
  }
  ValidationResult run(const Description &d, Target t, std::size_t depth) {
    tick(depth);
    detail::checkDescription(d);
    detail::checkPortableDescription(d, limits, work);
    if (!d.flag("visible") || d.flag("readOnly"))
      return {};
    if (t.value) {
      detail::checkRuntimeValue(*t.value, limits, work);
      references(*t.value, depth);
    }
    ValidationContext context(d, limits);
    own(d, t, context, depth);
    if (d.isContainer()) {
      if (auto m = model(t)) {
        auto children = enumerationChildren(d, limits);
        for (auto &child : children) {
          tick(depth);
          bool identity =
              child->isContainer() && !m->usesStoredContainerValues() &&
              !child->nativeRead &&
              ((!child->accessor && !child->accessorObject) ||
               child->effectiveAccessor().className == "MAIdentityAccessor");
          if (identity)
            context.include(run(*child, {nullptr, m}, depth + 1));
          else {
            auto value = m->readUsing(*child);
            context.include(run(*child, {&value}, depth + 1));
          }
        }
      } else if (t.value && !t.value->isNil())
        throw Error("Container value requires an object reference");
    }
    return context.finish();
  }
};
} // namespace
ValidationResult validateValueDetailed(const Description &d, const Value &v,
                                       const Graph *g, Limits l) {
  Validator validator{g, l, 0, {}};
  visit(
      d,
      [&](const auto &n) {
        detail::checkDescription(n);
        detail::checkPortableDescription(n, l, validator.work);
      },
      l);
  return validator.run(d, {&v}, 0);
}
ValidationResult validateDetailed(const Description &d, const Model &m,
                                  Limits l, const Graph *g) {
  Validator validator{g, l, 0, {}};
  visit(
      d,
      [&](const auto &n) {
        detail::checkDescription(n);
        detail::checkPortableDescription(n, l, validator.work);
      },
      l);
  if (d.isContainer())
    return validator.run(d, {nullptr, &m}, 0);
  if (!d.flag("visible") || d.flag("readOnly"))
    return {};
  auto v = m.readUsing(d);
  return validator.run(d, {&v}, 0);
}
bool isSatisfiedBy(const Description &d, const Value &v, const Graph *g,
                   Limits l) {
  return validateValueDetailed(d, v, g, l).passed();
}
bool isSatisfiedBy(const Description &d, const Model &m, Limits l,
                   const Graph *g) {
  return validateDetailed(d, m, l, g).passed();
}
} // namespace magritte
