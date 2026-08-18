/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_USERS_ANS42_CODE_RMDB_FIX_LOAD_SRC_PARSER_YACC_TAB_H_INCLUDED
# define YY_YY_USERS_ANS42_CODE_RMDB_FIX_LOAD_SRC_PARSER_YACC_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif
/* "%code requires" blocks.  */
#line 18 "/Users/ans42/Code/RMDB-fix-load/src/parser/yacc.y"

#include "parser_context.h"
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

#line 57 "/Users/ans42/Code/RMDB-fix-load/src/parser/yacc.tab.h"

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    SHOW = 258,                    /* SHOW  */
    TABLES = 259,                  /* TABLES  */
    CREATE = 260,                  /* CREATE  */
    TABLE = 261,                   /* TABLE  */
    DROP = 262,                    /* DROP  */
    DESC = 263,                    /* DESC  */
    INSERT = 264,                  /* INSERT  */
    INTO = 265,                    /* INTO  */
    VALUES = 266,                  /* VALUES  */
    DELETE = 267,                  /* DELETE  */
    FROM = 268,                    /* FROM  */
    ASC = 269,                     /* ASC  */
    ORDER = 270,                   /* ORDER  */
    BY = 271,                      /* BY  */
    WHERE = 272,                   /* WHERE  */
    UPDATE = 273,                  /* UPDATE  */
    SET = 274,                     /* SET  */
    TRANSACTION = 275,             /* TRANSACTION  */
    ISOLATION = 276,               /* ISOLATION  */
    LEVEL = 277,                   /* LEVEL  */
    SNAPSHOT = 278,                /* SNAPSHOT  */
    SERIALIZABLE = 279,            /* SERIALIZABLE  */
    SELECT = 280,                  /* SELECT  */
    INT = 281,                     /* INT  */
    CHAR = 282,                    /* CHAR  */
    FLOAT = 283,                   /* FLOAT  */
    DATETIME = 284,                /* DATETIME  */
    INDEX = 285,                   /* INDEX  */
    AND = 286,                     /* AND  */
    JOIN = 287,                    /* JOIN  */
    SEMI = 288,                    /* SEMI  */
    ON = 289,                      /* ON  */
    GROUP = 290,                   /* GROUP  */
    HAVING = 291,                  /* HAVING  */
    LIMIT = 292,                   /* LIMIT  */
    AS = 293,                      /* AS  */
    EXPLAIN = 294,                 /* EXPLAIN  */
    ANALYZE = 295,                 /* ANALYZE  */
    UNION = 296,                   /* UNION  */
    EXIT = 297,                    /* EXIT  */
    HELP = 298,                    /* HELP  */
    TXN_BEGIN = 299,               /* TXN_BEGIN  */
    TXN_COMMIT = 300,              /* TXN_COMMIT  */
    TXN_ABORT = 301,               /* TXN_ABORT  */
    TXN_ROLLBACK = 302,            /* TXN_ROLLBACK  */
    ORDER_BY = 303,                /* ORDER_BY  */
    ENABLE_NESTLOOP = 304,         /* ENABLE_NESTLOOP  */
    ENABLE_SORTMERGE = 305,        /* ENABLE_SORTMERGE  */
    STATIC_CHECKPOINT = 306,       /* STATIC_CHECKPOINT  */
    LOAD = 307,                    /* LOAD  */
    MAX = 308,                     /* MAX  */
    MIN = 309,                     /* MIN  */
    COUNT = 310,                   /* COUNT  */
    SUM = 311,                     /* SUM  */
    AVG = 312,                     /* AVG  */
    DISTINCT = 313,                /* DISTINCT  */
    LEQ = 314,                     /* LEQ  */
    NEQ = 315,                     /* NEQ  */
    GEQ = 316,                     /* GEQ  */
    T_EOF = 317,                   /* T_EOF  */
    IDENTIFIER = 318,              /* IDENTIFIER  */
    VALUE_STRING = 319,            /* VALUE_STRING  */
    VALUE_INT = 320,               /* VALUE_INT  */
    PARAMETER = 321,               /* PARAMETER  */
    VALUE_FLOAT = 322,             /* VALUE_FLOAT  */
    VALUE_BOOL = 323               /* VALUE_BOOL  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif




int yyparse (rmdb::ParserContext *context, yyscan_t scanner);


#endif /* !YY_YY_USERS_ANS42_CODE_RMDB_FIX_LOAD_SRC_PARSER_YACC_TAB_H_INCLUDED  */
