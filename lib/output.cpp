/* -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */

/*
 *  Main authors:
 *     Guido Tack <guido.tack@monash.edu>
 */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <minizinc/output.hh>
#include <minizinc/astiterator.hh>

namespace MiniZinc {

  void outputVarDecls(EnvI& env, Item* ci, Expression* e);
  
  bool cannotUseRHSForOutput(EnvI& env, Expression* e) {
    if (e==NULL)
      return true;
    
    class V : public EVisitor {
    public:
      EnvI& env;
      bool success;
      V(EnvI& env0) : env(env0), success(true) {}
      /// Visit anonymous variable
      void vAnonVar(const AnonVar&) { success = false; }
      /// Visit array literal
      void vArrayLit(const ArrayLit&) {}
      /// Visit array access
      void vArrayAccess(const ArrayAccess&) {}
      /// Visit array comprehension
      void vComprehension(const Comprehension&) {}
      /// Visit if-then-else
      void vITE(const ITE&) {}
      /// Visit binary operator
      void vBinOp(const BinOp&) {}
      /// Visit unary operator
      void vUnOp(const UnOp&) {}
      /// Visit call
      void vCall(Call& c) {
        std::vector<Type> tv(c.args().size());
        for (unsigned int i=c.args().size(); i--;) {
          tv[i] = c.args()[i]->type();
          tv[i].ti(Type::TI_PAR);
        }
        FunctionI* decl = env.output->matchFn(env,c.id(), tv);
        Type t;
        if (decl==NULL) {
          FunctionI* origdecl = env.orig->matchFn(env, c.id(), tv);
          if (origdecl == NULL) {
            throw FlatteningError(env,c.loc(),"function "+c.id().str()+" is used in output, par version needed");
          }
          
          if (origdecl->e() && cannotUseRHSForOutput(env, origdecl->e())) {
            success = false;
          } else {
            if (!origdecl->from_stdlib()) {
              decl = copy(env,env.cmap,origdecl)->cast<FunctionI>();
              CollectOccurrencesE ce(env.output_vo,decl);
              topDown(ce, decl->e());
              topDown(ce, decl->ti());
              for (unsigned int i = decl->params().size(); i--;)
                topDown(ce, decl->params()[i]);
              env.output->registerFn(env, decl);
              env.output->addItem(decl);
              outputVarDecls(env,origdecl,decl->e());
              outputVarDecls(env,origdecl,decl->ti());
            } else {
              decl = origdecl;
            }
            c.decl(decl);
          }
        }
        if (success) {
          t = decl->rtype(env, tv);
          if (!t.ispar())
            success = false;
        }
      }
      void vId(const Id& id) {}
      /// Visit let
      void vLet(const Let&) { success = false; }
      /// Visit variable declaration
      void vVarDecl(const VarDecl& vd) {}
      /// Visit type inst
      void vTypeInst(const TypeInst&) {}
      /// Visit TIId
      void vTIId(const TIId&) {}
      /// Determine whether to enter node
      bool enter(Expression* e) { return success; }
    } _v(env);
    topDown(_v, e);
    
    return !_v.success;
  }
  
  void removeIsOutput(VarDecl* vd) {
    if (vd==NULL)
      return;
    vd->ann().remove(constants().ann.output_var);
    vd->ann().removeCall(constants().ann.output_array);
  }
  
  void makePar(EnvI& env, Expression* e) {
    class Par : public EVisitor {
    public:
      /// Visit variable declaration
      void vVarDecl(VarDecl& vd) {
        vd.ti()->type(vd.type());
      }
      /// Determine whether to enter node
      bool enter(Expression* e) {
        Type t = e->type();
        t.ti(Type::TI_PAR);
        e->type(t);
        return true;
      }
    } _par;
    topDown(_par, e);
    class Decls : public EVisitor {
    protected:
      static std::string createEnumToStringName(Id* ident, std::string prefix) {
        std::string name = ident->str().str();
        if (name[0]=='\'') {
          name = "'"+prefix+name.substr(1);
        } else {
          name = prefix+name;
        }
        return name;
      }
      
    public:
      EnvI& env;
      Decls(EnvI& env0) : env(env0) {}
      void vCall(Call& c) {
        if (c.id()=="format" || c.id()=="show") {
          if (c.args()[c.args().size()-1]->type().enumId() > 0) {
            Id* ti_id = env.getEnum(c.args()[c.args().size()-1]->type().enumId())->e()->id();
            GCLock lock;
            std::vector<Expression*> args(1);
            args[0] = c.args()[c.args().size()-1];
            if (args[0]->type().dim() > 1) {
              Call* array1d = new Call(Location().introduce(),ASTString("array1d"),args);
              Type array1dt = args[0]->type();
              array1dt.dim(1);
              array1d->type(array1dt);
              args[0] = array1d;
            }
            std::string enumName = createEnumToStringName(ti_id, "_toString_");
            Call* convertEnum = new Call(Location().introduce(),enumName,args);
            convertEnum->type(Type::parstring());
            c.args()[c.args().size()-1] = convertEnum;
          }
        }
        c.decl(env.orig->matchFn(env,&c));
      }
    } _decls(env);
    topDown(_decls, e);
  }
  
  void checkRenameVar(EnvI& e, VarDecl* vd) {
    if (vd->id()->idn() != vd->flat()->id()->idn()) {
      TypeInst* vd_rename_ti = copy(e,e.cmap,vd->ti())->cast<TypeInst>();
      VarDecl* vd_rename = new VarDecl(Location().introduce(), vd_rename_ti, vd->flat()->id()->idn(), NULL);
      vd_rename->flat(vd->flat());
      vd->e(vd_rename->id());
      e.output->addItem(new VarDeclI(Location().introduce(), vd_rename));
    }
  }
  
  void outputVarDecls(EnvI& env, Item* ci, Expression* e) {
    class O : public EVisitor {
    public:
      EnvI& env;
      Item* ci;
      O(EnvI& env0, Item* ci0) : env(env0), ci(ci0) {}
      void vId(Id& id) {
        if (&id==constants().absent)
          return;
        if (!id.decl()->toplevel())
          return;
        VarDecl* vd = id.decl();
        VarDecl* reallyFlat = vd->flat();
        while (reallyFlat != NULL && reallyFlat != reallyFlat->flat())
          reallyFlat = reallyFlat->flat();
        IdMap<int>::iterator idx = reallyFlat ? env.output_vo.idx.find(reallyFlat->id()) : env.output_vo.idx.end();
        IdMap<int>::iterator idx2 = env.output_vo.idx.find(vd->id());
        if (idx==env.output_vo.idx.end() && idx2==env.output_vo.idx.end()) {
          VarDeclI* nvi = new VarDeclI(Location().introduce(), copy(env,env.cmap,vd)->cast<VarDecl>());
          Type t = nvi->e()->ti()->type();
          if (t.ti() != Type::TI_PAR) {
            t.ti(Type::TI_PAR);
          }
          makePar(env,nvi->e());
          nvi->e()->ti()->domain(NULL);
          nvi->e()->flat(vd->flat());
          nvi->e()->ann().clear();
          nvi->e()->introduced(false);
          if (reallyFlat)
            env.output_vo.add(reallyFlat, env.output->size());
          env.output_vo.add(nvi, env.output->size());
          env.output_vo.add(nvi->e(), ci);
          env.output->addItem(nvi);
          
          IdMap<KeepAlive>::iterator it;
          if ( (it = env.reverseMappers.find(nvi->e()->id())) != env.reverseMappers.end()) {
            Call* rhs = copy(env,env.cmap,it->second())->cast<Call>();
            {
              std::vector<Type> tv(rhs->args().size());
              for (unsigned int i=rhs->args().size(); i--;) {
                tv[i] = rhs->args()[i]->type();
                tv[i].ti(Type::TI_PAR);
              }
              FunctionI* decl = env.output->matchFn(env, rhs->id(), tv);
              Type t;
              if (decl==NULL) {
                FunctionI* origdecl = env.orig->matchFn(env, rhs->id(), tv);
                if (origdecl == NULL) {
                  throw FlatteningError(env,rhs->loc(),"function "+rhs->id().str()+" is used in output, par version needed");
                }
                if (!origdecl->from_stdlib()) {
                  decl = copy(env,env.cmap,origdecl)->cast<FunctionI>();
                  CollectOccurrencesE ce(env.output_vo,decl);
                  topDown(ce, decl->e());
                  topDown(ce, decl->ti());
                  for (unsigned int i = decl->params().size(); i--;)
                    topDown(ce, decl->params()[i]);
                  env.output->registerFn(env, decl);
                  env.output->addItem(decl);
                } else {
                  decl = origdecl;
                }
              }
              rhs->decl(decl);
            }
            outputVarDecls(env,nvi,it->second());
            nvi->e()->e(rhs);
          } else if (reallyFlat && cannotUseRHSForOutput(env, reallyFlat->e())) {
            assert(nvi->e()->flat());
            nvi->e()->e(NULL);
            if (nvi->e()->type().dim() == 0) {
              reallyFlat->addAnnotation(constants().ann.output_var);
            } else {
              std::vector<Expression*> args(reallyFlat->e()->type().dim());
              for (unsigned int i=0; i<args.size(); i++) {
                if (nvi->e()->ti()->ranges()[i]->domain() == NULL) {
                  args[i] = new SetLit(Location().introduce(), eval_intset(env,reallyFlat->ti()->ranges()[i]->domain()));
                } else {
                  args[i] = new SetLit(Location().introduce(), eval_intset(env,nvi->e()->ti()->ranges()[i]->domain()));
                }
              }
              ArrayLit* al = new ArrayLit(Location().introduce(), args);
              args.resize(1);
              args[0] = al;
              reallyFlat->addAnnotation(new Call(Location().introduce(),constants().ann.output_array,args,NULL));
            }
            checkRenameVar(env, nvi->e());
          } else {
            outputVarDecls(env, nvi, nvi->e()->ti());
            outputVarDecls(env, nvi, nvi->e()->e());
          }
          CollectOccurrencesE ce(env.output_vo,nvi);
          topDown(ce, nvi->e());
        }
      }
    } _o(env,ci);
    topDown(_o, e);
  }

  void createOutput(EnvI& e, std::vector<VarDecl*>& deletedFlatVarDecls) {
    if (e.output->size() > 0) {
      // Adapt existing output model
      // (generated by repeated flattening)
      e.output_vo.clear();
      for (unsigned int i=0; i<e.output->size(); i++) {
        Item* item = (*e.output)[i];
        if (item->removed())
          continue;
        switch (item->iid()) {
          case Item::II_VD:
          {
            VarDecl* vd = item->cast<VarDeclI>()->e();
            IdMap<KeepAlive>::iterator it;
            GCLock lock;
            VarDecl* reallyFlat = vd->flat();
            while (reallyFlat && reallyFlat!=reallyFlat->flat())
              reallyFlat=reallyFlat->flat();
            if (vd->e()==NULL) {
              if (vd->flat()->e() && vd->flat()->e()->type().ispar()) {
                VarDecl* reallyFlat = vd->flat();
                while (reallyFlat!=reallyFlat->flat())
                  reallyFlat=reallyFlat->flat();
                removeIsOutput(reallyFlat);
                Expression* flate = copy(e,e.cmap,follow_id(reallyFlat->id()));
                outputVarDecls(e,item,flate);
                vd->e(flate);
              } else if ( (it = e.reverseMappers.find(vd->id())) != e.reverseMappers.end()) {
                Call* rhs = copy(e,e.cmap,it->second())->cast<Call>();
                std::vector<Type> tv(rhs->args().size());
                for (unsigned int i=rhs->args().size(); i--;) {
                  tv[i] = rhs->args()[i]->type();
                  tv[i].ti(Type::TI_PAR);
                }
                FunctionI* decl = e.output->matchFn(e, rhs->id(), tv);
                if (decl==NULL) {
                  FunctionI* origdecl = e.orig->matchFn(e, rhs->id(), tv);
                  if (origdecl == NULL) {
                    throw FlatteningError(e,rhs->loc(),"function "+rhs->id().str()+" is used in output, par version needed");
                  }
                  if (!origdecl->from_stdlib()) {
                    decl = copy(e,e.cmap,origdecl)->cast<FunctionI>();
                    CollectOccurrencesE ce(e.output_vo,decl);
                    topDown(ce, decl->e());
                    topDown(ce, decl->ti());
                    for (unsigned int i = decl->params().size(); i--;)
                      topDown(ce, decl->params()[i]);
                    e.output->registerFn(e, decl);
                    e.output->addItem(decl);
                  } else {
                    decl = origdecl;
                  }
                }
                rhs->decl(decl);
                removeIsOutput(reallyFlat);
                
                if (e.vo.occurrences(reallyFlat)==0 && reallyFlat->e()==NULL) {
                  deletedFlatVarDecls.push_back(reallyFlat);
                }
                
                outputVarDecls(e,item,it->second()->cast<Call>());
                vd->e(rhs);
              } else {
                // If the VarDecl does not have a usable right hand side, it needs to be
                // marked as output in the FlatZinc
                assert(vd->flat());
                
                bool needOutputAnn = true;
                if (reallyFlat->e() && reallyFlat->e()->isa<ArrayLit>()) {
                  ArrayLit* al = reallyFlat->e()->cast<ArrayLit>();
                  for (unsigned int i=0; i<al->v().size(); i++) {
                    if (Id* id = al->v()[i]->dyn_cast<Id>()) {
                      if (e.reverseMappers.find(id) != e.reverseMappers.end()) {
                        needOutputAnn = false;
                        break;
                      }
                    }
                  }
                  if (!needOutputAnn) {
                    removeIsOutput(vd);
                    removeIsOutput(reallyFlat);
                    if (e.vo.occurrences(reallyFlat)==0) {
                      deletedFlatVarDecls.push_back(reallyFlat);
                    }
                    
                    outputVarDecls(e, item, al);
                    vd->e(copy(e,e.cmap,al));
                  }
                }
                if (needOutputAnn) {
                  if (!isOutput(vd->flat())) {
                    GCLock lock;
                    if (vd->type().dim() == 0) {
                      vd->flat()->addAnnotation(constants().ann.output_var);
                    } else {
                      std::vector<Expression*> args(vd->type().dim());
                      for (unsigned int i=0; i<args.size(); i++) {
                        if (vd->ti()->ranges()[i]->domain() == NULL) {
                          args[i] = new SetLit(Location().introduce(), eval_intset(e,vd->flat()->ti()->ranges()[i]->domain()));
                        } else {
                          args[i] = new SetLit(Location().introduce(), eval_intset(e,vd->ti()->ranges()[i]->domain()));
                        }
                      }
                      ArrayLit* al = new ArrayLit(Location().introduce(), args);
                      args.resize(1);
                      args[0] = al;
                      vd->flat()->addAnnotation(new Call(Location().introduce(),constants().ann.output_array,args,NULL));
                    }
                    checkRenameVar(e, vd);
                  }
                }
              }
              vd->flat(NULL);
            }
            e.output_vo.add(item->cast<VarDeclI>(), i);
            CollectOccurrencesE ce(e.output_vo,item);
            topDown(ce, vd);
          }
            break;
          case Item::II_OUT:
          {
            CollectOccurrencesE ce(e.output_vo,item);
            topDown(ce, item->cast<OutputI>()->e());
          }
            break;
          case Item::II_FUN:
          {
            CollectOccurrencesE ce(e.output_vo,item);
            topDown(ce, item->cast<FunctionI>()->e());
            topDown(ce, item->cast<FunctionI>()->ti());
            for (unsigned int i = item->cast<FunctionI>()->params().size(); i--;)
              topDown(ce, item->cast<FunctionI>()->params()[i]);
          }
            break;
          default:
            throw FlatteningError(e,item->loc(), "invalid item in output model");
        }
      }
    } else {
      // Create new output model
      OutputI* outputItem = NULL;
      GCLock lock;
      
      class OV1 : public ItemVisitor {
      public:
        EnvI& env;
        VarOccurrences& vo;
        OutputI*& outputItem;
        OV1(EnvI& env0, VarOccurrences& vo0, OutputI*& outputItem0)
        : env(env0), vo(vo0), outputItem(outputItem0) {}
        void vOutputI(OutputI* oi) {
          outputItem = copy(env,env.cmap, oi)->cast<OutputI>();
          makePar(env,outputItem->e());
          env.output->addItem(outputItem);
        }
      } _ov1(e,e.output_vo,outputItem);
      iterItems(_ov1,e.orig);
      
      if (outputItem==NULL) {
        // Create output item for all variables defined at toplevel in the MiniZinc source,
        // or those that are annotated with add_to_output
        std::vector<Expression*> outputVars;
        bool had_add_to_output = false;
        for (unsigned int i=0; i<e.orig->size(); i++) {
          if (VarDeclI* vdi = (*e.orig)[i]->dyn_cast<VarDeclI>()) {
            VarDecl* vd = vdi->e();
            bool process_var = false;
            if (vd->ann().contains(constants().ann.add_to_output)) {
              if (!had_add_to_output) {
                outputVars.clear();
              }
              had_add_to_output = true;
              process_var = true;
            } else {
              if (!had_add_to_output) {
                process_var = vd->type().isvar() && vd->e()==NULL;
              }
            }
            if (process_var) {
              std::ostringstream s;
              s << vd->id()->str().str() << " = ";
              if (vd->type().dim() > 0) {
                s << "array" << vd->type().dim() << "d(";
                if (vd->e() != NULL) {
                  ArrayLit* al = eval_array_lit(e, vd->e());
                  for (unsigned int i=0; i<al->dims(); i++) {
                    s << al->min(i) << ".." << al->max(i) << ",";
                  }
                } else {
                  for (unsigned int i=0; i<vd->type().dim(); i++) {
                    IntSetVal* idxset = eval_intset(e,vd->ti()->ranges()[i]->domain());
                    s << *idxset << ",";
                  }
                }
              }
              StringLit* sl = new StringLit(Location().introduce(),s.str());
              outputVars.push_back(sl);
              
              std::vector<Expression*> showArgs(1);
              showArgs[0] = vd->id();
              Call* show = new Call(Location().introduce(),constants().ids.show,showArgs);
              show->type(Type::parstring());
              FunctionI* fi = e.orig->matchFn(e, show);
              assert(fi);
              show->decl(fi);
              outputVars.push_back(show);
              std::string ends = vd->type().dim() > 0 ? ")" : "";
              ends += ";\n";
              StringLit* eol = new StringLit(Location().introduce(),ends);
              outputVars.push_back(eol);
            }
          }
        }
        OutputI* newOutputItem = new OutputI(Location().introduce(),new ArrayLit(Location().introduce(),outputVars));
        e.orig->addItem(newOutputItem);
        outputItem = copy(e,e.cmap, newOutputItem)->cast<OutputI>();
        makePar(e,outputItem->e());
        e.output->addItem(outputItem);
      }
      
      class CollectFunctions : public EVisitor {
      public:
        EnvI& env;
        CollectFunctions(EnvI& env0) : env(env0) {}
        bool enter(Expression* e) {
          if (e->type().isvar()) {
            Type t = e->type();
            t.ti(Type::TI_PAR);
            e->type(t);
          }
          return true;
        }
        void vCall(Call& c) {
          std::vector<Type> tv(c.args().size());
          for (unsigned int i=c.args().size(); i--;) {
            tv[i] = c.args()[i]->type();
            tv[i].ti(Type::TI_PAR);
          }
          FunctionI* decl = env.output->matchFn(env, c.id(), tv);
          Type t;
          if (decl==NULL) {
            FunctionI* origdecl = env.orig->matchFn(env, c.id(), tv);
            if (origdecl == NULL || !origdecl->rtype(env, tv).ispar()) {
              throw FlatteningError(env,c.loc(),"function "+c.id().str()+" is used in output, par version needed");
            }
            if (!origdecl->from_stdlib()) {
              decl = copy(env,env.cmap,origdecl)->cast<FunctionI>();
              CollectOccurrencesE ce(env.output_vo,decl);
              topDown(ce, decl->e());
              topDown(ce, decl->ti());
              for (unsigned int i = decl->params().size(); i--;)
                topDown(ce, decl->params()[i]);
              env.output->registerFn(env, decl);
              env.output->addItem(decl);
              if (decl->e())
                topDown(*this, decl->e());
            } else {
              decl = origdecl;
            }
          }
          c.decl(decl);
        }
      } _cf(e);
      topDown(_cf, outputItem->e());
      
      class OV2 : public ItemVisitor {
      public:
        EnvI& env;
        OV2(EnvI& env0) : env(env0) {}
        void vVarDeclI(VarDeclI* vdi) {
          IdMap<int>::iterator idx = env.output_vo.idx.find(vdi->e()->id());
          if (idx!=env.output_vo.idx.end())
            return;
          if (Expression* vd_e = env.cmap.find(vdi->e())) {
            VarDecl* vd = vd_e->cast<VarDecl>();
            VarDeclI* vdi_copy = copy(env,env.cmap,vdi)->cast<VarDeclI>();
            
            Type t = vdi_copy->e()->ti()->type();
            t.ti(Type::TI_PAR);
            vdi_copy->e()->ti()->domain(NULL);
            vdi_copy->e()->flat(vdi->e()->flat());
            vdi_copy->e()->ann().clear();
            vdi_copy->e()->introduced(false);
            IdMap<KeepAlive>::iterator it;
            if (!vdi->e()->type().ispar()) {
              if (vd->flat() == NULL && vdi->e()->e()!=NULL && vdi->e()->e()->type().ispar()) {
                Expression* flate = eval_par(env, vdi->e()->e());
                outputVarDecls(env,vdi_copy,flate);
                vd->e(flate);
              } else {
                vd = follow_id_to_decl(vd->id())->cast<VarDecl>();
                VarDecl* reallyFlat = vd->flat();
                
                while (reallyFlat!=reallyFlat->flat())
                  reallyFlat=reallyFlat->flat();
                if (vd->flat()->e() && vd->flat()->e()->type().ispar()) {
                  Expression* flate = copy(env,env.cmap,follow_id(reallyFlat->id()));
                  outputVarDecls(env,vdi_copy,flate);
                  vd->e(flate);
                } else if ( (it = env.reverseMappers.find(vd->id())) != env.reverseMappers.end()) {
                  Call* rhs = copy(env,env.cmap,it->second())->cast<Call>();
                  {
                    std::vector<Type> tv(rhs->args().size());
                    for (unsigned int i=rhs->args().size(); i--;) {
                      tv[i] = rhs->args()[i]->type();
                      tv[i].ti(Type::TI_PAR);
                    }
                    FunctionI* decl = env.output->matchFn(env, rhs->id(), tv);
                    if (decl==NULL) {
                      FunctionI* origdecl = env.orig->matchFn(env, rhs->id(), tv);
                      if (origdecl == NULL) {
                        throw FlatteningError(env,rhs->loc(),"function "+rhs->id().str()+" is used in output, par version needed");
                      }
                      if (!origdecl->from_stdlib()) {
                        decl = copy(env,env.cmap,origdecl)->cast<FunctionI>();
                        CollectOccurrencesE ce(env.output_vo,decl);
                        topDown(ce, decl->e());
                        topDown(ce, decl->ti());
                        for (unsigned int i = decl->params().size(); i--;)
                          topDown(ce, decl->params()[i]);
                        env.output->registerFn(env, decl);
                        env.output->addItem(decl);
                      } else {
                        decl = origdecl;
                      }
                    }
                    rhs->decl(decl);
                  }
                  outputVarDecls(env,vdi_copy,rhs);
                  vd->e(rhs);
                } else if (cannotUseRHSForOutput(env,vd->e())) {
                  // If the VarDecl does not have a usable right hand side, it needs to be
                  // marked as output in the FlatZinc
                  vd->e(NULL);
                  assert(vd->flat());
                  if (vd->type().dim() == 0) {
                    vd->flat()->addAnnotation(constants().ann.output_var);
                    checkRenameVar(env, vd);
                  } else {
                    bool needOutputAnn = true;
                    if (reallyFlat->e() && reallyFlat->e()->isa<ArrayLit>()) {
                      ArrayLit* al = reallyFlat->e()->cast<ArrayLit>();
                      for (unsigned int i=0; i<al->v().size(); i++) {
                        if (Id* id = al->v()[i]->dyn_cast<Id>()) {
                          if (env.reverseMappers.find(id) != env.reverseMappers.end()) {
                            needOutputAnn = false;
                            break;
                          }
                        }
                      }
                      if (!needOutputAnn) {
                        outputVarDecls(env, vdi_copy, al);
                        vd->e(copy(env,env.cmap,al));
                      }
                    }
                    if (needOutputAnn) {
                      std::vector<Expression*> args(vdi->e()->type().dim());
                      for (unsigned int i=0; i<args.size(); i++) {
                        if (vdi->e()->ti()->ranges()[i]->domain() == NULL) {
                          args[i] = new SetLit(Location().introduce(), eval_intset(env,vd->flat()->ti()->ranges()[i]->domain()));
                        } else {
                          args[i] = new SetLit(Location().introduce(), eval_intset(env,vd->ti()->ranges()[i]->domain()));
                        }
                      }
                      ArrayLit* al = new ArrayLit(Location().introduce(), args);
                      args.resize(1);
                      args[0] = al;
                      vd->flat()->addAnnotation(new Call(Location().introduce(),constants().ann.output_array,args,NULL));
                      checkRenameVar(env, vd);
                    }
                  }
                }
                if (env.output_vo.find(reallyFlat) == -1)
                  env.output_vo.add(reallyFlat, env.output->size());
              }
            }
            makePar(env,vdi_copy->e());
            env.output_vo.add(vdi_copy, env.output->size());
            CollectOccurrencesE ce(env.output_vo,vdi_copy);
            topDown(ce, vdi_copy->e());
            env.output->addItem(vdi_copy);
          }
        }
      } _ov2(e);
      iterItems(_ov2,e.orig);
      
      CollectOccurrencesE ce(e.output_vo,outputItem);
      topDown(ce, outputItem->e());
      
      e.orig->mergeStdLib(e, e.output);
    }
    
    std::vector<VarDecl*> deletedVarDecls;
    for (unsigned int i=0; i<e.output->size(); i++) {
      if (VarDeclI* vdi = (*e.output)[i]->dyn_cast<VarDeclI>()) {
        if (!vdi->removed() && e.output_vo.occurrences(vdi->e())==0) {
          CollectDecls cd(e.output_vo,deletedVarDecls,vdi);
          topDown(cd, vdi->e()->e());
          removeIsOutput(vdi->e()->flat());
          if (e.output_vo.find(vdi->e())!=-1)
            e.output_vo.remove(vdi->e());
          vdi->remove();
        }
      }
    }
    while (!deletedVarDecls.empty()) {
      VarDecl* cur = deletedVarDecls.back(); deletedVarDecls.pop_back();
      if (e.output_vo.occurrences(cur) == 0) {
        IdMap<int>::iterator cur_idx = e.output_vo.idx.find(cur->id());
        if (cur_idx != e.output_vo.idx.end()) {
          VarDeclI* vdi = (*e.output)[cur_idx->second]->cast<VarDeclI>();
          if (!vdi->removed()) {
            CollectDecls cd(e.output_vo,deletedVarDecls,vdi);
            topDown(cd,cur->e());
            removeIsOutput(vdi->e()->flat());
            if (e.output_vo.find(vdi->e())!=-1)
              e.output_vo.remove(vdi->e());
            vdi->remove();
          }
        }
      }
    }
    
    for (IdMap<VarOccurrences::Items>::iterator it = e.output_vo._m.begin();
         it != e.output_vo._m.end(); ++it) {
      std::vector<Item*> toRemove;
      for (VarOccurrences::Items::iterator iit = it->second.begin();
           iit != it->second.end(); ++iit) {
        if ((*iit)->removed()) {
          toRemove.push_back(*iit);
        }
      }
      for (unsigned int i=0; i<toRemove.size(); i++) {
        it->second.erase(toRemove[i]);
      }
    }
  }
  
}