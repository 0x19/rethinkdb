// Autogenerated by metajava.py.
// Do not edit this file directly.
// The template for this file is located at:
// ../../../../../../../../templates/AstSubclass.java
package com.rethinkdb.ast.gen;

import com.rethinkdb.model.Arguments;
import com.rethinkdb.model.OptArgs;
import com.rethinkdb.ast.ReqlAst;
import com.rethinkdb.proto.TermType;


public class Min extends ReqlQuery {


    public Min(java.lang.Object arg) {
        this(new Arguments(arg), null);
    }
    public Min(Arguments args, OptArgs optargs) {
        this(null, args, optargs);
    }
    public Min(ReqlAst prev, Arguments args, OptArgs optargs) {
        this(prev, TermType.MIN, args, optargs);
    }
    protected Min(ReqlAst previous, TermType termType, Arguments args, OptArgs optargs){
        super(previous, termType, args, optargs);
    }


    /* Static factories */
    public static Min fromArgs(java.lang.Object... args){
        return new Min(new Arguments(args), null);
    }


}