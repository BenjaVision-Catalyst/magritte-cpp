#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
using namespace magritte;
using J=nlohmann::json;
int checks=0;
void check(bool b,const std::string& message){if(!b)throw Error(message);++checks;}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const Error&){failed=true;}check(failed,"Expected rejection");}
J fixture(const char* name){std::ifstream in(std::string(FIXTURES)+"/"+name+".json");J j;in>>j;return j;}
Value value(const J& j){return decodeValue(j.dump());}
J wire(const Value& v){return J::parse(encodeValue(v));}
std::shared_ptr<Description> field(const std::string& id,const std::string& cls){auto d=std::make_shared<Description>();d->id=id;d->className=cls;d->key=id;d->properties["label"]=Value(id=="reference"?"Reference":"Choice");return d;}
int main(int argc,char** argv){try{
 auto f=fixture("dynamic-options-reference");
 std::vector<std::string> state{"alpha","alpine","beta","Alpha"}; J calls=J::array(),queries=J::array();
 auto source=std::make_shared<DynamicOptions>([&](const std::string& query){queries.push_back(query);Value::Array values;for(const auto& s:state)if(s.rfind(query,0)==0)values.emplace_back(s);return Value(values);});
 source->includesBlock([&](const Value& v){calls.push_back(wire(v));return v==Value("member")||v==Value(std::int64_t(42));});
 auto reference=field("reference","MANumberDescription");reference->properties["min"]=Value(std::int64_t(10));reference->properties["max"]=Value(std::int64_t(20));
 auto choice=field("choice","MASingleOptionDescription");choice->reference=reference;choice->options=source;
 for(std::size_t i=0;i<f.at("queries").size();++i){if(i==4)state.push_back("altar");const auto& q=f.at("queries")[i];check(choice->allOptions().selectMatching(q.at("query"))==value(q.at("value")),"GT completion");}
 check(source->detect(Value("alpine"))==value(f["readers"][0]["value"]),"Default identity reader");
 source->reader([](const Value& v){auto text=std::get<std::string>(v.data);for(auto& c:text)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return Value(text);});
 check(source->detect(Value("alpine"))==value(f["readers"][1]["value"]),"Custom reader");
 auto copied=copyDescription(choice,"copied");auto clone=source->copy();
 check(copied.root->options.provider()!=source&&copied.root->referenceDescription()!=reference,"GT option description postCopy");
 check(clone!=source&&clone->block()==source->block()&&clone->includesBlock()==source->includesBlock(),"Shallow provider copy shares blocks");
 source->reader([](const Value& v){return Value("new-"+std::get<std::string>(v.data));});
 check(copied.root->options.provider()->detect(Value("alpine"))==value(f["readers"][2]["value"]),"Copied reader slot independent");
 check(source->detect(Value("alpine"))==value(f["readers"][3]["value"]),"Original reader replaced");
 check(choice->allOptions().provider()==source&&&source->allOptionsFor(*choice)==source.get(),"allOptions preserves identity");
 for(const auto& row:f.at("allOptions")){
  choice->className=row["class"];choice->properties["required"]=Value(row["required"].get<bool>());choice->properties["extensible"]=Value(row["extensible"].get<bool>());choice->properties["sorted"]=Value(row["sorted"].get<bool>());
  calls=J::array();queries=J::array();check(choice->allOptions().provider()==source,"No eager preparation");check(calls==row["includes"]&&queries==row["queries"],"Flags do not invoke callbacks");
 }
 for(const auto& row:f.at("validation")){
  choice->className=row["class"];choice->properties["required"]=Value(row["required"].get<bool>());choice->properties["extensible"]=Value(row["extensible"].get<bool>());
  calls=J::array();queries=J::array();J errors=J::array();
  for(const auto& e:validateValue(*choice,value(row["input"])))errors.push_back({{"class",e.code},{"description",e.descriptionId=="choice"?"Choice":"Reference"}});
  check(errors==row["errors"],"GT validation "+row.dump());check(calls==row["includes"]&&queries==row["queries"],"GT membership invocation order");
 }
 auto prepSource=std::make_shared<DynamicOptions>();prepSource->includesBlock([&](const Value& v){calls.push_back(std::get<std::string>(v.data));return v==Value("known");});
 choice->className="MASingleOptionDescription";choice->options=prepSource;
 for(const auto& row:f.at("allOptionsWith")){
  choice->className=row["class"];
  choice->properties["required"]=Value(row["required"].get<bool>());choice->properties["extensible"]=Value(row["extensible"].get<bool>());choice->properties["sorted"]=Value(row["sorted"].get<bool>());
  auto current=row["current"]=="nil"?Value():Value(row["current"].get<std::string>());calls=J::array();
  if(row["result"][0]=="MessageNotUnderstood")rejects([&]{choice->allOptionsWith(current);});
  else{auto result=choice->allOptionsWith(current);check(result.size()==row["result"].size(),"GT provider collection shape");
   auto provider=std::get<std::shared_ptr<DynamicOptions>>(result.entries().back());check(provider!=prepSource&&provider->includesBlock()==prepSource->includesBlock(),"asArray contains copied provider");
   if(result.size()==2)check(std::get<Value>(result.entries().front()).isNil(),"Undefined entry before copied provider");
   rejects([&]{result.values();});}
  check(calls==row["calls"],"GT allOptionsWith membership short circuit");
 }
 Options fixed{Value("alpha"),Value("alpine"),Value("beta"),Value("Alpha")};
 for(const auto& row:f.at("staticMatching"))check(fixed.selectMatching(row["query"])==value(row["value"]),"Pinned collection prefix semantics with GT beginsWith projection");
 // Native callback/resource boundaries do not materialize or execute hidden provider behavior.
 choice->options=source;choice->properties["required"]=Value(false);choice->properties["extensible"]=Value(false);
 rejects([&]{choice->allOptions().values();});auto missing=std::make_shared<DynamicOptions>();
 rejects([&]{missing->selectMatching("x");});rejects([&]{missing->includes(Value("x"));});check(missing->detect(Value())==Value(),"Nil identity reader");
 auto scalar=std::make_shared<DynamicOptions>([](const std::string&){return Value(std::int64_t(3));});check(scalar->selectMatching("x")==Value(std::int64_t(3)),"Completion result is not coerced to collection");
 auto mutableBlock=std::make_shared<DynamicOptions>([n=std::int64_t(0)](const std::string&)mutable{return Value(++n);});auto sharedBlock=mutableBlock->copy();
 check(mutableBlock->selectMatching("")==Value(std::int64_t(1))&&sharedBlock->selectMatching("")==Value(std::int64_t(2)),"Mutable C++ block state shared by shallow copies");
 auto replacing=std::make_shared<DynamicOptions>();std::weak_ptr<DynamicOptions> weak=replacing;
 replacing->block([weak](const std::string&){weak.lock()->block([](const std::string&){return Value(std::int64_t(2));});return Value(std::int64_t(1));});
 check(replacing->selectMatching("")==Value(std::int64_t(1))&&replacing->selectMatching("")==Value(std::int64_t(2)),"Executing completion survives slot replacement");
 replacing->includesBlock([weak](const Value&){weak.lock()->includesBlock([](const Value&){return false;});return true;});
 check(replacing->includes(Value("x"))&&!replacing->includes(Value("x")),"Executing membership survives slot replacement");
 replacing->reader([weak](const Value&){weak.lock()->reader({});return Value(std::int64_t(7));});
 check(replacing->detect(Value(std::int64_t(3)))==Value(std::int64_t(7))&&replacing->detect(Value(std::int64_t(3)))==Value(std::int64_t(3)),"Executing reader survives slot replacement");
 Options movedFrom(source);Options movedTo(std::move(movedFrom));check(movedTo.provider()==source,"Moved provider keeps identity");rejects([&]{movedFrom.provider();});
 Limits tiny;tiny.bytes=8;rejects([&]{source->selectMatching("long query",tiny);});
 tiny=Limits{};tiny.items=1;rejects([&]{source->selectMatching("",tiny);});
 auto bad=std::make_shared<DynamicOptions>([](const std::string&){throw Error("callback failed");return Value();});rejects([&]{bad->selectMatching("");});
 choice=field("choice","MASingleOptionDescription");choice->reference=reference;
 Graph graph;graph.format="magritte.graph/4";graph.root="app";auto app=field("app","MAContainer");app->properties["label"]=Value("Options");
 choice->options=source;auto many=field("many","MAMultipleOptionDescription");many->reference=reference;many->options=source;
 auto kind=field("kind","MAClassDescription");app->children={choice,many,kind};graph.descriptions={{"app",app},{"choice",choice},{"many",many},{"reference",reference},{"kind",kind}};
 queries=J::array();rejects([&]{graph.encode();});check(queries.empty(),"Export never executes provider");choice->properties["visible"]=Value(false);rejects([&]{graph.encode();});choice->properties["visible"]=Value(true);
 auto localQuery=std::make_shared<DynamicOptions>([](const std::string&){return Value(Value::Array{Value("member"),Value(std::int64_t(42))});});
 localQuery->includesBlock([](const Value& v){return v==Value("member")||v==Value(std::int64_t(42));});choice->options=localQuery;many->options=localQuery;
 AdaptiveModel sample;sample.id="sample";sample.description=app;sample.values={{"choice",Value(std::int64_t(42))},{"many",Value(Value::Array{Value(std::int64_t(42)),Value("member")})},{"kind",Value(Value::Class{"MADynamicOptions"})}};
 check(validate(*app,sample).empty(),"Local live providers drive ordinary adaptive validation");
 auto snapshot=localQuery->selectMatching("").elements();choice->options=snapshot;many->options=snapshot;graph.objects.emplace("sample",sample);
 check(validate(*app,graph.objects.at("sample")).empty(),"Explicit static replacement remains valid");
 auto metadata=RecursiveDescriptionModel();auto binding=metadata.bind(choice);auto schema=metadata.describeReflective(choice);auto text=metadata.resolver().descriptions.at(schema->id+".optionsTextual");
 choice->options=localQuery;rejects([&]{binding->readUsing(*text);});
 // Writing optionsTextual replaces the provider only after successful parsing.
 reference->className="MAStringDescription";reference->properties.erase("min");reference->properties.erase("max");
 binding->writeUsing(*text,Value("first\rsecond"));check(!choice->options.isDynamic()&&Value(choice->options.values())==value(f["textual"]["after"]),"GT textual setter replaces provider");
 choice->options=localQuery;reference->className="MANumberDescription";reference->properties["min"]=Value(std::int64_t(10));reference->properties["max"]=Value(std::int64_t(20));
 rejects([&]{binding->writeUsing(*text,Value("not-a-number"));});check(choice->options.provider()==localQuery,"Failed textual parsing preserves provider");choice->options=snapshot;
 auto info=ClassRegistry::global().find("MADynamicOptions");check(info&&info->superclass==f["class"]["superclass"]&&info->label==f["class"]["label"]&&!info->isAbstract,"GT dynamic provider class metadata");
 auto relation=field("relation","MAToOneRelationDescription");relation->reference=reference;
 RecursiveDescriptionModel registryMetadata;auto registrySchema=registryMetadata.describeReflective(relation);
 auto classOptions=registryMetadata.resolver().descriptions.at(registrySchema->id+".classes")->options.values();
 check(std::find(classOptions.begin(),classOptions.end(),Value(Value::Class{"MADynamicOptions"}))!=classOptions.end(),"Fresh metadata discovers provider class");
 auto expectedRoundtrip=fixture("dynamic-options-roundtrip-reference");
 check(writeString(*app,sample)==expectedRoundtrip[0]["reads"][0]["text"],"GT string writer after explicit static replacement");
 auto encoded=graph.encode();check(Graph::decode(encoded).encode()==encoded,"Static graph remains graph/4");
 if(argc>1){std::ofstream out(argv[1]);out<<encoded;}
 std::cout<<checks<<" dynamic-options checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"After "<<checks<<" checks: "<<e.what()<<"\n";return 1;}}
