/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 2

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 1 "yacc.y"

#include "ast.h"
#include "yacc.tab.h"
#include <iostream>
#include <memory>
#include <string>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc, yyscan_t scanner);

void yyerror(YYLTYPE *locp, rmdb::ParserContext *context, yyscan_t, const char* s) {
    context->error = "Parser error at line " + std::to_string(locp->first_line) +
                     " column " + std::to_string(locp->first_column) + ": " + s;
}

using namespace ast;

#line 88 "yacc.tab.cpp"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "yacc.tab.hpp"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_SHOW = 3,                       /* SHOW  */
  YYSYMBOL_TABLES = 4,                     /* TABLES  */
  YYSYMBOL_CREATE = 5,                     /* CREATE  */
  YYSYMBOL_TABLE = 6,                      /* TABLE  */
  YYSYMBOL_DROP = 7,                       /* DROP  */
  YYSYMBOL_DESC = 8,                       /* DESC  */
  YYSYMBOL_INSERT = 9,                     /* INSERT  */
  YYSYMBOL_INTO = 10,                      /* INTO  */
  YYSYMBOL_VALUES = 11,                    /* VALUES  */
  YYSYMBOL_DELETE = 12,                    /* DELETE  */
  YYSYMBOL_FROM = 13,                      /* FROM  */
  YYSYMBOL_ASC = 14,                       /* ASC  */
  YYSYMBOL_ORDER = 15,                     /* ORDER  */
  YYSYMBOL_BY = 16,                        /* BY  */
  YYSYMBOL_WHERE = 17,                     /* WHERE  */
  YYSYMBOL_UPDATE = 18,                    /* UPDATE  */
  YYSYMBOL_SET = 19,                       /* SET  */
  YYSYMBOL_TRANSACTION = 20,               /* TRANSACTION  */
  YYSYMBOL_ISOLATION = 21,                 /* ISOLATION  */
  YYSYMBOL_LEVEL = 22,                     /* LEVEL  */
  YYSYMBOL_SNAPSHOT = 23,                  /* SNAPSHOT  */
  YYSYMBOL_SERIALIZABLE = 24,              /* SERIALIZABLE  */
  YYSYMBOL_SELECT = 25,                    /* SELECT  */
  YYSYMBOL_INT = 26,                       /* INT  */
  YYSYMBOL_CHAR = 27,                      /* CHAR  */
  YYSYMBOL_FLOAT = 28,                     /* FLOAT  */
  YYSYMBOL_DATETIME = 29,                  /* DATETIME  */
  YYSYMBOL_INDEX = 30,                     /* INDEX  */
  YYSYMBOL_AND = 31,                       /* AND  */
  YYSYMBOL_JOIN = 32,                      /* JOIN  */
  YYSYMBOL_SEMI = 33,                      /* SEMI  */
  YYSYMBOL_ON = 34,                        /* ON  */
  YYSYMBOL_GROUP = 35,                     /* GROUP  */
  YYSYMBOL_HAVING = 36,                    /* HAVING  */
  YYSYMBOL_LIMIT = 37,                     /* LIMIT  */
  YYSYMBOL_AS = 38,                        /* AS  */
  YYSYMBOL_EXPLAIN = 39,                   /* EXPLAIN  */
  YYSYMBOL_ANALYZE = 40,                   /* ANALYZE  */
  YYSYMBOL_UNION = 41,                     /* UNION  */
  YYSYMBOL_EXIT = 42,                      /* EXIT  */
  YYSYMBOL_HELP = 43,                      /* HELP  */
  YYSYMBOL_TXN_BEGIN = 44,                 /* TXN_BEGIN  */
  YYSYMBOL_TXN_COMMIT = 45,                /* TXN_COMMIT  */
  YYSYMBOL_TXN_ABORT = 46,                 /* TXN_ABORT  */
  YYSYMBOL_TXN_ROLLBACK = 47,              /* TXN_ROLLBACK  */
  YYSYMBOL_ORDER_BY = 48,                  /* ORDER_BY  */
  YYSYMBOL_ENABLE_NESTLOOP = 49,           /* ENABLE_NESTLOOP  */
  YYSYMBOL_ENABLE_SORTMERGE = 50,          /* ENABLE_SORTMERGE  */
  YYSYMBOL_STATIC_CHECKPOINT = 51,         /* STATIC_CHECKPOINT  */
  YYSYMBOL_LOAD = 52,                      /* LOAD  */
  YYSYMBOL_MAX = 53,                       /* MAX  */
  YYSYMBOL_MIN = 54,                       /* MIN  */
  YYSYMBOL_COUNT = 55,                     /* COUNT  */
  YYSYMBOL_SUM = 56,                       /* SUM  */
  YYSYMBOL_AVG = 57,                       /* AVG  */
  YYSYMBOL_DISTINCT = 58,                  /* DISTINCT  */
  YYSYMBOL_LEQ = 59,                       /* LEQ  */
  YYSYMBOL_NEQ = 60,                       /* NEQ  */
  YYSYMBOL_GEQ = 61,                       /* GEQ  */
  YYSYMBOL_T_EOF = 62,                     /* T_EOF  */
  YYSYMBOL_IDENTIFIER = 63,                /* IDENTIFIER  */
  YYSYMBOL_VALUE_STRING = 64,              /* VALUE_STRING  */
  YYSYMBOL_VALUE_INT = 65,                 /* VALUE_INT  */
  YYSYMBOL_PARAMETER = 66,                 /* PARAMETER  */
  YYSYMBOL_VALUE_FLOAT = 67,               /* VALUE_FLOAT  */
  YYSYMBOL_VALUE_BOOL = 68,                /* VALUE_BOOL  */
  YYSYMBOL_69_ = 69,                       /* ';'  */
  YYSYMBOL_70_ = 70,                       /* '='  */
  YYSYMBOL_71_ = 71,                       /* '('  */
  YYSYMBOL_72_ = 72,                       /* ')'  */
  YYSYMBOL_73_ = 73,                       /* ','  */
  YYSYMBOL_74_ = 74,                       /* '+'  */
  YYSYMBOL_75_ = 75,                       /* '-'  */
  YYSYMBOL_76_ = 76,                       /* '.'  */
  YYSYMBOL_77_ = 77,                       /* '<'  */
  YYSYMBOL_78_ = 78,                       /* '>'  */
  YYSYMBOL_79_ = 79,                       /* '*'  */
  YYSYMBOL_80_ = 80,                       /* '/'  */
  YYSYMBOL_YYACCEPT = 81,                  /* $accept  */
  YYSYMBOL_start = 82,                     /* start  */
  YYSYMBOL_stmt = 83,                      /* stmt  */
  YYSYMBOL_txnStmt = 84,                   /* txnStmt  */
  YYSYMBOL_dbStmt = 85,                    /* dbStmt  */
  YYSYMBOL_setStmt = 86,                   /* setStmt  */
  YYSYMBOL_ddl = 87,                       /* ddl  */
  YYSYMBOL_dml = 88,                       /* dml  */
  YYSYMBOL_selectStmt = 89,                /* selectStmt  */
  YYSYMBOL_plainSelectStmt = 90,           /* plainSelectStmt  */
  YYSYMBOL_unionSelectList = 91,           /* unionSelectList  */
  YYSYMBOL_unionSelect = 92,               /* unionSelect  */
  YYSYMBOL_fieldList = 93,                 /* fieldList  */
  YYSYMBOL_colNameList = 94,               /* colNameList  */
  YYSYMBOL_field = 95,                     /* field  */
  YYSYMBOL_type = 96,                      /* type  */
  YYSYMBOL_valueList = 97,                 /* valueList  */
  YYSYMBOL_value = 98,                     /* value  */
  YYSYMBOL_condition = 99,                 /* condition  */
  YYSYMBOL_optWhereClause = 100,           /* optWhereClause  */
  YYSYMBOL_whereClause = 101,              /* whereClause  */
  YYSYMBOL_col = 102,                      /* col  */
  YYSYMBOL_colList = 103,                  /* colList  */
  YYSYMBOL_op = 104,                       /* op  */
  YYSYMBOL_expr = 105,                     /* expr  */
  YYSYMBOL_setClauses = 106,               /* setClauses  */
  YYSYMBOL_setClause = 107,                /* setClause  */
  YYSYMBOL_arithmeticSetClause = 108,      /* arithmeticSetClause  */
  YYSYMBOL_newSelector = 109,              /* newSelector  */
  YYSYMBOL_selectItemList = 110,           /* selectItemList  */
  YYSYMBOL_selectItem = 111,               /* selectItem  */
  YYSYMBOL_aggregateItem = 112,            /* aggregateItem  */
  YYSYMBOL_aggName = 113,                  /* aggName  */
  YYSYMBOL_fromList = 114,                 /* fromList  */
  YYSYMBOL_optJoinOnClause = 115,          /* optJoinOnClause  */
  YYSYMBOL_tableRef = 116,                 /* tableRef  */
  YYSYMBOL_opt_order_clause = 117,         /* opt_order_clause  */
  YYSYMBOL_order_clause = 118,             /* order_clause  */
  YYSYMBOL_order_item_list = 119,          /* order_item_list  */
  YYSYMBOL_order_item = 120,               /* order_item  */
  YYSYMBOL_opt_asc_desc = 121,             /* opt_asc_desc  */
  YYSYMBOL_optGroupClause = 122,           /* optGroupClause  */
  YYSYMBOL_optHavingClause = 123,          /* optHavingClause  */
  YYSYMBOL_havingClause = 124,             /* havingClause  */
  YYSYMBOL_havingCondition = 125,          /* havingCondition  */
  YYSYMBOL_havingLhs = 126,                /* havingLhs  */
  YYSYMBOL_optLimitClause = 127,           /* optLimitClause  */
  YYSYMBOL_set_knob_type = 128,            /* set_knob_type  */
  YYSYMBOL_tbName = 129,                   /* tbName  */
  YYSYMBOL_colName = 130                   /* colName  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if 1

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* 1 */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
             && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE) \
             + YYSIZEOF (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  62
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   261

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  81
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  50
/* YYNRULES -- Number of rules.  */
#define YYNRULES  142
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  262

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   323


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
      71,    72,    79,    74,    73,    75,    76,    80,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    69,
      77,    70,    78,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,    86,    86,    91,    96,   101,   109,   110,   111,   112,
     113,   117,   121,   125,   129,   136,   140,   147,   151,   155,
     162,   166,   170,   174,   178,   182,   189,   193,   197,   201,
     205,   212,   216,   220,   227,   235,   239,   247,   254,   258,
     265,   269,   276,   283,   287,   291,   295,   302,   306,   313,
     317,   321,   325,   329,   333,   337,   341,   345,   352,   359,
     360,   367,   371,   378,   382,   389,   393,   400,   404,   408,
     412,   416,   420,   427,   431,   438,   442,   449,   453,   457,
     464,   468,   472,   476,   480,   484,   488,   492,   496,   501,
     506,   511,   527,   531,   535,   539,   546,   550,   554,   558,
     566,   570,   574,   578,   582,   589,   593,   597,   601,   623,
     628,   633,   639,   650,   654,   658,   662,   666,   670,   674,
     681,   685,   689,   696,   700,   707,   714,   715,   716,   720,
     724,   728,   732,   736,   740,   747,   754,   761,   765,   769,
     770,   773,   775
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if 1
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "SHOW", "TABLES",
  "CREATE", "TABLE", "DROP", "DESC", "INSERT", "INTO", "VALUES", "DELETE",
  "FROM", "ASC", "ORDER", "BY", "WHERE", "UPDATE", "SET", "TRANSACTION",
  "ISOLATION", "LEVEL", "SNAPSHOT", "SERIALIZABLE", "SELECT", "INT",
  "CHAR", "FLOAT", "DATETIME", "INDEX", "AND", "JOIN", "SEMI", "ON",
  "GROUP", "HAVING", "LIMIT", "AS", "EXPLAIN", "ANALYZE", "UNION", "EXIT",
  "HELP", "TXN_BEGIN", "TXN_COMMIT", "TXN_ABORT", "TXN_ROLLBACK",
  "ORDER_BY", "ENABLE_NESTLOOP", "ENABLE_SORTMERGE", "STATIC_CHECKPOINT",
  "LOAD", "MAX", "MIN", "COUNT", "SUM", "AVG", "DISTINCT", "LEQ", "NEQ",
  "GEQ", "T_EOF", "IDENTIFIER", "VALUE_STRING", "VALUE_INT", "PARAMETER",
  "VALUE_FLOAT", "VALUE_BOOL", "';'", "'='", "'('", "')'", "','", "'+'",
  "'-'", "'.'", "'<'", "'>'", "'*'", "'/'", "$accept", "start", "stmt",
  "txnStmt", "dbStmt", "setStmt", "ddl", "dml", "selectStmt",
  "plainSelectStmt", "unionSelectList", "unionSelect", "fieldList",
  "colNameList", "field", "type", "valueList", "value", "condition",
  "optWhereClause", "whereClause", "col", "colList", "op", "expr",
  "setClauses", "setClause", "arithmeticSetClause", "newSelector",
  "selectItemList", "selectItem", "aggregateItem", "aggName", "fromList",
  "optJoinOnClause", "tableRef", "opt_order_clause", "order_clause",
  "order_item_list", "order_item", "opt_asc_desc", "optGroupClause",
  "optHavingClause", "havingClause", "havingCondition", "havingLhs",
  "optLimitClause", "set_knob_type", "tbName", "colName", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-198)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-142)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
     145,    22,    -1,    19,   -51,    41,    77,   -51,   -12,    21,
       8,  -198,  -198,  -198,  -198,  -198,  -198,    39,  -198,    72,
      35,  -198,  -198,  -198,  -198,  -198,  -198,  -198,  -198,    94,
     -51,   -51,  -198,   -51,   -51,  -198,  -198,   -51,   -51,    96,
     114,  -198,  -198,    51,  -198,  -198,    67,  -198,  -198,    75,
    -198,   111,   149,    98,  -198,   134,   102,   103,  -198,   152,
    -198,   164,  -198,  -198,   -51,   133,   142,  -198,   143,   171,
     188,   155,   193,   151,   -43,   155,   -16,    73,   155,   157,
     155,  -198,   -51,  -198,   155,   155,   155,   150,   157,  -198,
    -198,   -14,  -198,    37,   153,    30,  -198,     0,   154,   156,
    -198,   152,   -11,   189,    -6,  -198,  -198,   158,  -198,  -198,
      46,  -198,   167,    59,  -198,    83,   101,  -198,   194,    32,
     155,  -198,   101,   101,   101,   101,    31,   203,  -198,   157,
     159,  -198,  -198,  -198,   -27,  -198,   -16,   -16,   192,   197,
     -51,  -198,  -198,  -198,   155,  -198,   161,  -198,  -198,  -198,
    -198,   155,  -198,  -198,  -198,  -198,  -198,  -198,    16,    20,
      86,  -198,   157,  -198,  -198,  -198,  -198,  -198,  -198,   135,
    -198,  -198,  -198,  -198,  -198,   106,   137,  -198,   162,  -198,
     152,     6,   201,  -198,   221,   202,   -16,  -198,  -198,   174,
    -198,  -198,  -198,  -198,  -198,  -198,   101,  -198,  -198,  -198,
    -198,   155,   155,   155,   155,   101,   101,   101,   101,   168,
    -198,   -51,  -198,   157,  -198,   157,    90,   226,   208,   172,
    -198,  -198,  -198,  -198,  -198,  -198,  -198,  -198,  -198,  -198,
    -198,   194,  -198,   170,  -198,   214,  -198,    32,   230,   210,
     157,  -198,   157,    90,   101,   157,   183,  -198,  -198,  -198,
    -198,  -198,   100,  -198,   176,  -198,  -198,  -198,  -198,  -198,
     157,  -198
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     4,     3,    11,    12,    13,    14,     0,     5,     0,
       0,     9,     6,    10,     7,     8,    30,    31,    15,     0,
       0,     0,    25,     0,     0,   141,    22,     0,     0,     0,
       0,   139,   140,     0,   105,   106,     0,   107,   108,   142,
      92,    96,     0,    93,    94,    98,     0,     0,    64,     0,
      32,     0,     1,     2,     0,     0,     0,    21,     0,     0,
      59,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    33,     0,    16,     0,     0,     0,     0,     0,    28,
     142,    59,    75,    79,     0,     0,    17,     0,     0,     0,
      97,     0,    59,   109,   115,    95,    99,     0,    63,    27,
       0,    38,     0,     0,    40,     0,     0,    61,    60,     0,
       0,    29,     0,     0,     0,     0,     0,     0,    19,     0,
       0,   102,   101,    37,     0,    35,     0,     0,   130,     0,
       0,   116,   100,    20,     0,    43,     0,    45,    46,    42,
      23,     0,    24,    55,    49,    57,    52,    56,     0,     0,
       0,    47,     0,    71,    70,    72,    67,    68,    69,     0,
      76,    88,    89,    90,    91,    77,    78,    18,     0,   103,
       0,     0,   114,   110,     0,   132,     0,   117,    39,     0,
      41,    50,    53,    51,    54,    26,     0,    62,    73,    74,
      58,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      36,     0,   119,     0,   111,     0,     0,   121,     0,     0,
      48,    84,    85,    86,    87,    80,    81,    82,    83,   104,
     118,   113,    65,   129,   136,   131,   133,     0,     0,   138,
       0,    44,     0,     0,     0,     0,     0,    34,   112,    66,
     134,   135,   128,   120,   122,   123,   137,   127,   126,   125,
       0,   124
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -198,  -198,  -198,  -198,  -198,  -198,  -198,  -198,  -198,     7,
    -198,    70,  -198,   166,   109,  -198,  -198,   -83,  -160,   -35,
      42,    -9,  -198,    17,  -198,  -198,   136,  -198,  -198,  -198,
     180,  -197,  -198,  -198,  -198,  -126,  -198,  -198,  -198,    -2,
    -198,  -198,  -198,  -198,    18,  -198,  -198,  -198,    -3,   -62
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,    19,    20,    21,    22,    23,    24,    25,    26,   133,
     134,   135,   110,   113,   111,   149,   160,   161,   117,    89,
     118,   119,   233,   169,   200,    91,    92,    93,    52,    53,
      54,    55,    56,   102,   214,   103,   239,   253,   254,   255,
     259,   185,   217,   235,   236,   237,   247,    43,    57,    58
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      51,    36,   197,    88,    39,    30,    88,    27,    40,    94,
     182,   183,    35,   100,   180,    97,   106,    60,   108,   234,
      49,   136,   112,   114,   114,    33,    28,    65,    66,    31,
      67,    68,   140,     9,    69,    70,    98,    41,    42,   171,
     172,   173,   174,   175,   211,   181,   234,    35,    59,    34,
      32,    37,    29,   127,   128,   101,   121,    35,    94,   120,
     218,    83,   137,    49,   176,    99,    81,   138,    51,    35,
     107,   129,    62,   104,    44,    45,    46,    47,    48,   109,
     248,   191,   112,   192,    49,   193,   198,   194,   130,   190,
      38,   163,   164,   165,    90,   153,   154,   155,   156,   157,
      50,   141,   166,    61,    63,   158,   159,    64,   257,   167,
     168,   122,   123,   220,   258,    71,   124,   125,   143,   144,
     178,    73,   225,   226,   227,   228,    44,    45,    46,    47,
      48,   150,   151,   104,   104,    72,    49,   187,    74,   221,
     222,   223,   224,    44,    45,    46,    47,    48,     1,    75,
       2,  -141,     3,     4,     5,   152,   151,     6,   195,   196,
     199,   251,    76,     7,     8,   153,   154,   155,   156,   157,
       9,    77,    78,    79,    82,   158,   159,     9,   212,    80,
     201,   202,    87,   104,    10,   203,   204,    11,    12,    13,
      14,    15,    16,   145,   146,   147,   148,    17,    49,   153,
     154,   155,   156,   157,    84,    88,   232,    18,   230,   158,
     159,   205,   206,    85,    86,    95,   207,   208,    90,    96,
      49,   116,   139,   126,   177,   162,   131,   184,   132,   186,
     142,   179,   189,   249,   209,   213,   252,   215,   216,   219,
     229,   238,   240,   242,   241,   243,   245,   246,   256,   260,
     210,   252,   115,   188,   244,   231,   170,   105,   261,     0,
       0,   250
};

static const yytype_int16 yycheck[] =
{
       9,     4,   162,    17,     7,     6,    17,     0,    20,    71,
     136,   137,    63,    75,    41,    58,    78,    10,    80,   216,
      63,    32,    84,    85,    86,     6,     4,    30,    31,    30,
      33,    34,    38,    25,    37,    38,    79,    49,    50,   122,
     123,   124,   125,   126,    38,    72,   243,    63,    40,    30,
      51,    10,    30,    23,    24,    71,    91,    63,   120,    73,
     186,    64,    73,    63,   126,    74,    59,   102,    77,    63,
      79,    71,     0,    76,    53,    54,    55,    56,    57,    82,
     240,    65,   144,    67,    63,    65,   169,    67,    97,   151,
      13,    59,    60,    61,    63,    64,    65,    66,    67,    68,
      79,   104,    70,    64,    69,    74,    75,    13,     8,    77,
      78,    74,    75,   196,    14,    19,    79,    80,    72,    73,
     129,    70,   205,   206,   207,   208,    53,    54,    55,    56,
      57,    72,    73,   136,   137,    21,    63,   140,    71,   201,
     202,   203,   204,    53,    54,    55,    56,    57,     3,    38,
       5,    76,     7,     8,     9,    72,    73,    12,    72,    73,
     169,   244,    13,    18,    19,    64,    65,    66,    67,    68,
      25,    73,    38,    71,    10,    74,    75,    25,   181,    76,
      74,    75,    11,   186,    39,    79,    80,    42,    43,    44,
      45,    46,    47,    26,    27,    28,    29,    52,    63,    64,
      65,    66,    67,    68,    71,    17,   215,    62,   211,    74,
      75,    74,    75,    71,    71,    22,    79,    80,    63,    68,
      63,    71,    33,    70,    21,    31,    72,    35,    72,    32,
      72,    72,    71,   242,    72,    34,   245,    16,    36,    65,
      72,    15,    34,    73,    72,    31,    16,    37,    65,    73,
     180,   260,    86,   144,   237,   213,   120,    77,   260,    -1,
      -1,   243
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,     3,     5,     7,     8,     9,    12,    18,    19,    25,
      39,    42,    43,    44,    45,    46,    47,    52,    62,    82,
      83,    84,    85,    86,    87,    88,    89,    90,     4,    30,
       6,    30,    51,     6,    30,    63,   129,    10,    13,   129,
      20,    49,    50,   128,    53,    54,    55,    56,    57,    63,
      79,   102,   109,   110,   111,   112,   113,   129,   130,    40,
      90,    64,     0,    69,    13,   129,   129,   129,   129,   129,
     129,    19,    21,    70,    71,    38,    13,    73,    38,    71,
      76,    90,    10,   129,    71,    71,    71,    11,    17,   100,
      63,   106,   107,   108,   130,    22,    68,    58,    79,   102,
     130,    71,   114,   116,   129,   111,   130,   102,   130,   129,
      93,    95,   130,    94,   130,    94,    71,    99,   101,   102,
      73,   100,    74,    75,    79,    80,    70,    23,    24,    71,
     102,    72,    72,    90,    91,    92,    32,    73,   100,    33,
      38,   129,    72,    72,    73,    26,    27,    28,    29,    96,
      72,    73,    72,    64,    65,    66,    67,    68,    74,    75,
      97,    98,    31,    59,    60,    61,    70,    77,    78,   104,
     107,    98,    98,    98,    98,    98,   130,    21,   102,    72,
      41,    72,   116,   116,    35,   122,    32,   129,    95,    71,
     130,    65,    67,    65,    67,    72,    73,    99,    98,   102,
     105,    74,    75,    79,    80,    74,    75,    79,    80,    72,
      92,    38,   129,    34,   115,    16,    36,   123,   116,    65,
      98,   130,   130,   130,   130,    98,    98,    98,    98,    72,
     129,   101,   102,   103,   112,   124,   125,   126,    15,   117,
      34,    72,    73,    31,   104,    16,    37,   127,    99,   102,
     125,    98,   102,   118,   119,   120,    65,     8,    14,   121,
      73,   120
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_uint8 yyr1[] =
{
       0,    81,    82,    82,    82,    82,    83,    83,    83,    83,
      83,    84,    84,    84,    84,    85,    85,    86,    86,    86,
      87,    87,    87,    87,    87,    87,    88,    88,    88,    88,
      88,    89,    89,    89,    90,    91,    91,    92,    93,    93,
      94,    94,    95,    96,    96,    96,    96,    97,    97,    98,
      98,    98,    98,    98,    98,    98,    98,    98,    99,   100,
     100,   101,   101,   102,   102,   103,   103,   104,   104,   104,
     104,   104,   104,   105,   105,   106,   106,   107,   107,   107,
     108,   108,   108,   108,   108,   108,   108,   108,   108,   108,
     108,   108,   109,   109,   110,   110,   111,   111,   111,   111,
     112,   112,   112,   112,   112,   113,   113,   113,   113,   114,
     114,   114,   114,   115,   115,   116,   116,   116,   116,   116,
     117,   117,   118,   119,   119,   120,   121,   121,   121,   122,
     122,   123,   123,   124,   124,   125,   126,   127,   127,   128,
     128,   129,   130
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     2,     4,     4,     6,     5,
       6,     3,     2,     6,     6,     2,     7,     4,     4,     5,
       1,     1,     2,     3,     9,     1,     3,     1,     1,     3,
       1,     3,     2,     1,     4,     1,     1,     1,     3,     1,
       2,     2,     1,     2,     2,     1,     1,     1,     3,     0,
       2,     1,     3,     3,     1,     1,     3,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     3,     3,     3,     1,
       5,     5,     5,     5,     5,     5,     5,     5,     3,     3,
       3,     3,     1,     1,     1,     3,     1,     3,     1,     3,
       4,     4,     4,     5,     7,     1,     1,     1,     1,     1,
       3,     4,     6,     2,     0,     1,     2,     3,     5,     4,
       3,     0,     1,     1,     3,     2,     1,     1,     0,     3,
       0,     2,     0,     1,     3,     3,     1,     2,     0,     1,
       1,     1,     1
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (&yylloc, context, scanner, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF

/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = YYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = YYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = YYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            YYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            YYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)


/* YYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

# ifndef YYLOCATION_PRINT

#  if defined YY_LOCATION_PRINT

   /* Temporary convenience wrapper in case some people defined the
      undocumented and private YY_LOCATION_PRINT macros.  */
#   define YYLOCATION_PRINT(File, Loc)  YY_LOCATION_PRINT(File, *(Loc))

#  elif defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int
yy_location_print_ (FILE *yyo, YYLTYPE const * const yylocp)
{
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line)
    {
      res += YYFPRINTF (yyo, "%d", yylocp->first_line);
      if (0 <= yylocp->first_column)
        res += YYFPRINTF (yyo, ".%d", yylocp->first_column);
    }
  if (0 <= yylocp->last_line)
    {
      if (yylocp->first_line < yylocp->last_line)
        {
          res += YYFPRINTF (yyo, "-%d", yylocp->last_line);
          if (0 <= end_col)
            res += YYFPRINTF (yyo, ".%d", end_col);
        }
      else if (0 <= end_col && yylocp->first_column < end_col)
        res += YYFPRINTF (yyo, "-%d", end_col);
    }
  return res;
}

#   define YYLOCATION_PRINT  yy_location_print_

    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT(File, Loc)  YYLOCATION_PRINT(File, &(Loc))

#  else

#   define YYLOCATION_PRINT(File, Loc) ((void) 0)
    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT  YYLOCATION_PRINT

#  endif
# endif /* !defined YYLOCATION_PRINT */


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, Location, context, scanner); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, rmdb::ParserContext *context, yyscan_t scanner)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (yylocationp);
  YY_USE (context);
  YY_USE (scanner);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, rmdb::ParserContext *context, yyscan_t scanner)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  YYLOCATION_PRINT (yyo, yylocationp);
  YYFPRINTF (yyo, ": ");
  yy_symbol_value_print (yyo, yykind, yyvaluep, yylocationp, context, scanner);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, YYLTYPE *yylsp,
                 int yyrule, rmdb::ParserContext *context, yyscan_t scanner)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)],
                       &(yylsp[(yyi + 1) - (yynrhs)]), context, scanner);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, yylsp, Rule, context, scanner); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


/* Context of a parse error.  */
typedef struct
{
  yy_state_t *yyssp;
  yysymbol_kind_t yytoken;
  YYLTYPE *yylloc;
} yypcontext_t;

/* Put in YYARG at most YYARGN of the expected tokens given the
   current YYCTX, and return the number of tokens stored in YYARG.  If
   YYARG is null, return the number of expected tokens (guaranteed to
   be less than YYNTOKENS).  Return YYENOMEM on memory exhaustion.
   Return 0 if there are more than YYARGN expected tokens, yet fill
   YYARG up to YYARGN. */
static int
yypcontext_expected_tokens (const yypcontext_t *yyctx,
                            yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  int yyn = yypact[+*yyctx->yyssp];
  if (!yypact_value_is_default (yyn))
    {
      /* Start YYX at -YYN if negative to avoid negative indexes in
         YYCHECK.  In other words, skip the first -YYN actions for
         this state because they are default actions.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;
      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yyx;
      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
        if (yycheck[yyx + yyn] == yyx && yyx != YYSYMBOL_YYerror
            && !yytable_value_is_error (yytable[yyx + yyn]))
          {
            if (!yyarg)
              ++yycount;
            else if (yycount == yyargn)
              return 0;
            else
              yyarg[yycount++] = YY_CAST (yysymbol_kind_t, yyx);
          }
    }
  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = YYSYMBOL_YYEMPTY;
  return yycount;
}




#ifndef yystrlen
# if defined __GLIBC__ && defined _STRING_H
#  define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
# else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
# endif
#endif

#ifndef yystpcpy
# if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#  define yystpcpy stpcpy
# else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
# endif
#endif

#ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;
      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
#endif


static int
yy_syntax_error_arguments (const yypcontext_t *yyctx,
                           yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yyctx->yytoken != YYSYMBOL_YYEMPTY)
    {
      int yyn;
      if (yyarg)
        yyarg[yycount] = yyctx->yytoken;
      ++yycount;
      yyn = yypcontext_expected_tokens (yyctx,
                                        yyarg ? yyarg + 1 : yyarg, yyargn - 1);
      if (yyn == YYENOMEM)
        return YYENOMEM;
      else
        yycount += yyn;
    }
  return yycount;
}

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return -1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return YYENOMEM if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                const yypcontext_t *yyctx)
{
  enum { YYARGS_MAX = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  yysymbol_kind_t yyarg[YYARGS_MAX];
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* Actual size of YYARG. */
  int yycount = yy_syntax_error_arguments (yyctx, yyarg, YYARGS_MAX);
  if (yycount == YYENOMEM)
    return YYENOMEM;

  switch (yycount)
    {
#define YYCASE_(N, S)                       \
      case N:                               \
        yyformat = S;                       \
        break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

  /* Compute error message size.  Don't count the "%s"s, but reserve
     room for the terminator.  */
  yysize = yystrlen (yyformat) - 2 * yycount + 1;
  {
    int yyi;
    for (yyi = 0; yyi < yycount; ++yyi)
      {
        YYPTRDIFF_T yysize1
          = yysize + yytnamerr (YY_NULLPTR, yytname[yyarg[yyi]]);
        if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
          yysize = yysize1;
        else
          return YYENOMEM;
      }
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return -1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yytname[yyarg[yyi++]]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, YYLTYPE *yylocationp, rmdb::ParserContext *context, yyscan_t scanner)
{
  YY_USE (yyvaluep);
  YY_USE (yylocationp);
  YY_USE (context);
  YY_USE (scanner);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (rmdb::ParserContext *context, yyscan_t scanner)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

/* Location data for the lookahead symbol.  */
static YYLTYPE yyloc_default
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
YYLTYPE yylloc = yyloc_default;

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

    /* The location stack: array, bottom, top.  */
    YYLTYPE yylsa[YYINITDEPTH];
    YYLTYPE *yyls = yylsa;
    YYLTYPE *yylsp = yyls;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[3];

  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  yylsp[0] = yylloc;
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;
        YYLTYPE *yyls1 = yyls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yyls1, yysize * YYSIZEOF (*yylsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
        yyls = yyls1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
        YYSTACK_RELOCATE (yyls_alloc, yyls);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval, &yylloc, scanner);
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      yyerror_range[1] = yylloc;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location. */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 2: /* start: stmt ';'  */
#line 87 "yacc.y"
    {
        context->result = (yyvsp[-1].sv_node);
        YYACCEPT;
    }
#line 1800 "yacc.tab.cpp"
    break;

  case 3: /* start: HELP  */
#line 92 "yacc.y"
    {
        context->result = std::make_shared<Help>();
        YYACCEPT;
    }
#line 1809 "yacc.tab.cpp"
    break;

  case 4: /* start: EXIT  */
#line 97 "yacc.y"
    {
        context->result.reset();
        YYACCEPT;
    }
#line 1818 "yacc.tab.cpp"
    break;

  case 5: /* start: T_EOF  */
#line 102 "yacc.y"
    {
        context->result.reset();
        YYACCEPT;
    }
#line 1827 "yacc.tab.cpp"
    break;

  case 11: /* txnStmt: TXN_BEGIN  */
#line 118 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnBegin>();
    }
#line 1835 "yacc.tab.cpp"
    break;

  case 12: /* txnStmt: TXN_COMMIT  */
#line 122 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnCommit>();
    }
#line 1843 "yacc.tab.cpp"
    break;

  case 13: /* txnStmt: TXN_ABORT  */
#line 126 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnAbort>();
    }
#line 1851 "yacc.tab.cpp"
    break;

  case 14: /* txnStmt: TXN_ROLLBACK  */
#line 130 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<TxnRollback>();
    }
#line 1859 "yacc.tab.cpp"
    break;

  case 15: /* dbStmt: SHOW TABLES  */
#line 137 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowTables>();
    }
#line 1867 "yacc.tab.cpp"
    break;

  case 16: /* dbStmt: SHOW INDEX FROM tbName  */
#line 141 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ShowIndex>((yyvsp[0].sv_str));
    }
#line 1875 "yacc.tab.cpp"
    break;

  case 17: /* setStmt: SET set_knob_type '=' VALUE_BOOL  */
#line 148 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetStmt>((yyvsp[-2].sv_setKnobType), (yyvsp[0].sv_bool));
    }
#line 1883 "yacc.tab.cpp"
    break;

  case 18: /* setStmt: SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION  */
#line 152 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetTransactionIsolation>(false);
    }
#line 1891 "yacc.tab.cpp"
    break;

  case 19: /* setStmt: SET TRANSACTION ISOLATION LEVEL SERIALIZABLE  */
#line 156 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<SetTransactionIsolation>(true);
    }
#line 1899 "yacc.tab.cpp"
    break;

  case 20: /* ddl: CREATE TABLE tbName '(' fieldList ')'  */
#line 163 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateTable>((yyvsp[-3].sv_str), (yyvsp[-1].sv_fields));
    }
#line 1907 "yacc.tab.cpp"
    break;

  case 21: /* ddl: DROP TABLE tbName  */
#line 167 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropTable>((yyvsp[0].sv_str));
    }
#line 1915 "yacc.tab.cpp"
    break;

  case 22: /* ddl: DESC tbName  */
#line 171 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DescTable>((yyvsp[0].sv_str));
    }
#line 1923 "yacc.tab.cpp"
    break;

  case 23: /* ddl: CREATE INDEX tbName '(' colNameList ')'  */
#line 175 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1931 "yacc.tab.cpp"
    break;

  case 24: /* ddl: DROP INDEX tbName '(' colNameList ')'  */
#line 179 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DropIndex>((yyvsp[-3].sv_str), (yyvsp[-1].sv_strs));
    }
#line 1939 "yacc.tab.cpp"
    break;

  case 25: /* ddl: CREATE STATIC_CHECKPOINT  */
#line 183 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<CreateCheckpoint>();
    }
#line 1947 "yacc.tab.cpp"
    break;

  case 26: /* dml: INSERT INTO tbName VALUES '(' valueList ')'  */
#line 190 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<InsertStmt>((yyvsp[-4].sv_str), (yyvsp[-1].sv_vals));
    }
#line 1955 "yacc.tab.cpp"
    break;

  case 27: /* dml: LOAD VALUE_STRING INTO tbName  */
#line 194 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<LoadStmt>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 1963 "yacc.tab.cpp"
    break;

  case 28: /* dml: DELETE FROM tbName optWhereClause  */
#line 198 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<DeleteStmt>((yyvsp[-1].sv_str), (yyvsp[0].sv_conds));
    }
#line 1971 "yacc.tab.cpp"
    break;

  case 29: /* dml: UPDATE tbName SET setClauses optWhereClause  */
#line 202 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<UpdateStmt>((yyvsp[-3].sv_str), (yyvsp[-1].sv_set_clauses), (yyvsp[0].sv_conds));
    }
#line 1979 "yacc.tab.cpp"
    break;

  case 30: /* dml: selectStmt  */
#line 206 "yacc.y"
    {
        (yyval.sv_node) = (yyvsp[0].sv_node);
    }
#line 1987 "yacc.tab.cpp"
    break;

  case 31: /* selectStmt: plainSelectStmt  */
#line 213 "yacc.y"
    {
        (yyval.sv_node) = (yyvsp[0].sv_select_stmt);
    }
#line 1995 "yacc.tab.cpp"
    break;

  case 32: /* selectStmt: EXPLAIN plainSelectStmt  */
#line 217 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ExplainStmt>((yyvsp[0].sv_select_stmt));
    }
#line 2003 "yacc.tab.cpp"
    break;

  case 33: /* selectStmt: EXPLAIN ANALYZE plainSelectStmt  */
#line 221 "yacc.y"
    {
        (yyval.sv_node) = std::make_shared<ExplainStmt>((yyvsp[0].sv_select_stmt));
    }
#line 2011 "yacc.tab.cpp"
    break;

  case 34: /* plainSelectStmt: SELECT newSelector FROM fromList optWhereClause optGroupClause optHavingClause opt_order_clause optLimitClause  */
#line 228 "yacc.y"
    {
        (yyvsp[-4].sv_conds).insert((yyvsp[-4].sv_conds).end(), (yyvsp[-5].sv_from)->join_conds.begin(), (yyvsp[-5].sv_from)->join_conds.end());
        (yyval.sv_select_stmt) = std::make_shared<SelectStmt>((yyvsp[-7].sv_select_items), (yyvsp[-5].sv_from)->table_refs, (yyvsp[-4].sv_conds), (yyvsp[-3].sv_cols), (yyvsp[-2].sv_havings), (yyvsp[-1].sv_orderby), (yyvsp[0].sv_int), (yyvsp[-5].sv_from)->is_semi_join, (yyvsp[-5].sv_from)->semi_conds);
    }
#line 2020 "yacc.tab.cpp"
    break;

  case 35: /* unionSelectList: unionSelect  */
#line 236 "yacc.y"
    {
        (yyval.sv_select_stmts) = std::vector<std::shared_ptr<SelectStmt>>{(yyvsp[0].sv_select_stmt)};
    }
#line 2028 "yacc.tab.cpp"
    break;

  case 36: /* unionSelectList: unionSelectList UNION unionSelect  */
#line 240 "yacc.y"
    {
        (yyvsp[-2].sv_select_stmts).push_back((yyvsp[0].sv_select_stmt));
        (yyval.sv_select_stmts) = (yyvsp[-2].sv_select_stmts);
    }
#line 2037 "yacc.tab.cpp"
    break;

  case 37: /* unionSelect: plainSelectStmt  */
#line 248 "yacc.y"
    {
        (yyval.sv_select_stmt) = (yyvsp[0].sv_select_stmt);
    }
#line 2045 "yacc.tab.cpp"
    break;

  case 38: /* fieldList: field  */
#line 255 "yacc.y"
    {
        (yyval.sv_fields) = std::vector<std::shared_ptr<Field>>{(yyvsp[0].sv_field)};
    }
#line 2053 "yacc.tab.cpp"
    break;

  case 39: /* fieldList: fieldList ',' field  */
#line 259 "yacc.y"
    {
        (yyval.sv_fields).push_back((yyvsp[0].sv_field));
    }
#line 2061 "yacc.tab.cpp"
    break;

  case 40: /* colNameList: colName  */
#line 266 "yacc.y"
    {
        (yyval.sv_strs) = std::vector<std::string>{(yyvsp[0].sv_str)};
    }
#line 2069 "yacc.tab.cpp"
    break;

  case 41: /* colNameList: colNameList ',' colName  */
#line 270 "yacc.y"
    {
        (yyval.sv_strs).push_back((yyvsp[0].sv_str));
    }
#line 2077 "yacc.tab.cpp"
    break;

  case 42: /* field: colName type  */
#line 277 "yacc.y"
    {
        (yyval.sv_field) = std::make_shared<ColDef>((yyvsp[-1].sv_str), (yyvsp[0].sv_type_len));
    }
#line 2085 "yacc.tab.cpp"
    break;

  case 43: /* type: INT  */
#line 284 "yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
#line 2093 "yacc.tab.cpp"
    break;

  case 44: /* type: CHAR '(' VALUE_INT ')'  */
#line 288 "yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_STRING, (yyvsp[-1].sv_int));
    }
#line 2101 "yacc.tab.cpp"
    break;

  case 45: /* type: FLOAT  */
#line 292 "yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
#line 2109 "yacc.tab.cpp"
    break;

  case 46: /* type: DATETIME  */
#line 296 "yacc.y"
    {
        (yyval.sv_type_len) = std::make_shared<TypeLen>(SV_TYPE_STRING, 19);
    }
#line 2117 "yacc.tab.cpp"
    break;

  case 47: /* valueList: value  */
#line 303 "yacc.y"
    {
        (yyval.sv_vals) = std::vector<std::shared_ptr<Value>>{(yyvsp[0].sv_val)};
    }
#line 2125 "yacc.tab.cpp"
    break;

  case 48: /* valueList: valueList ',' value  */
#line 307 "yacc.y"
    {
        (yyval.sv_vals).push_back((yyvsp[0].sv_val));
    }
#line 2133 "yacc.tab.cpp"
    break;

  case 49: /* value: VALUE_INT  */
#line 314 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>((yyvsp[0].sv_int));
    }
#line 2141 "yacc.tab.cpp"
    break;

  case 50: /* value: '+' VALUE_INT  */
#line 318 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>((yyvsp[0].sv_int));
    }
#line 2149 "yacc.tab.cpp"
    break;

  case 51: /* value: '-' VALUE_INT  */
#line 322 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<IntLit>(-(yyvsp[0].sv_int));
    }
#line 2157 "yacc.tab.cpp"
    break;

  case 52: /* value: VALUE_FLOAT  */
#line 326 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>((yyvsp[0].sv_float));
    }
#line 2165 "yacc.tab.cpp"
    break;

  case 53: /* value: '+' VALUE_FLOAT  */
#line 330 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>((yyvsp[0].sv_float));
    }
#line 2173 "yacc.tab.cpp"
    break;

  case 54: /* value: '-' VALUE_FLOAT  */
#line 334 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<FloatLit>(-(yyvsp[0].sv_float));
    }
#line 2181 "yacc.tab.cpp"
    break;

  case 55: /* value: VALUE_STRING  */
#line 338 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<StringLit>((yyvsp[0].sv_str));
    }
#line 2189 "yacc.tab.cpp"
    break;

  case 56: /* value: VALUE_BOOL  */
#line 342 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<BoolLit>((yyvsp[0].sv_bool));
    }
#line 2197 "yacc.tab.cpp"
    break;

  case 57: /* value: PARAMETER  */
#line 346 "yacc.y"
    {
        (yyval.sv_val) = std::make_shared<ParameterRef>(static_cast<uint32_t>((yyvsp[0].sv_int)));
    }
#line 2205 "yacc.tab.cpp"
    break;

  case 58: /* condition: col op expr  */
#line 353 "yacc.y"
    {
        (yyval.sv_cond) = std::make_shared<BinaryExpr>((yyvsp[-2].sv_col), (yyvsp[-1].sv_comp_op), (yyvsp[0].sv_expr));
    }
#line 2213 "yacc.tab.cpp"
    break;

  case 59: /* optWhereClause: %empty  */
#line 359 "yacc.y"
                      { (yyval.sv_conds) = {}; }
#line 2219 "yacc.tab.cpp"
    break;

  case 60: /* optWhereClause: WHERE whereClause  */
#line 361 "yacc.y"
    {
        (yyval.sv_conds) = (yyvsp[0].sv_conds);
    }
#line 2227 "yacc.tab.cpp"
    break;

  case 61: /* whereClause: condition  */
#line 368 "yacc.y"
    {
        (yyval.sv_conds) = std::vector<std::shared_ptr<BinaryExpr>>{(yyvsp[0].sv_cond)};
    }
#line 2235 "yacc.tab.cpp"
    break;

  case 62: /* whereClause: whereClause AND condition  */
#line 372 "yacc.y"
    {
        (yyval.sv_conds).push_back((yyvsp[0].sv_cond));
    }
#line 2243 "yacc.tab.cpp"
    break;

  case 63: /* col: tbName '.' colName  */
#line 379 "yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2251 "yacc.tab.cpp"
    break;

  case 64: /* col: colName  */
#line 383 "yacc.y"
    {
        (yyval.sv_col) = std::make_shared<Col>("", (yyvsp[0].sv_str));
    }
#line 2259 "yacc.tab.cpp"
    break;

  case 65: /* colList: col  */
#line 390 "yacc.y"
    {
        (yyval.sv_cols) = std::vector<std::shared_ptr<Col>>{(yyvsp[0].sv_col)};
    }
#line 2267 "yacc.tab.cpp"
    break;

  case 66: /* colList: colList ',' col  */
#line 394 "yacc.y"
    {
        (yyval.sv_cols).push_back((yyvsp[0].sv_col));
    }
#line 2275 "yacc.tab.cpp"
    break;

  case 67: /* op: '='  */
#line 401 "yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_EQ;
    }
#line 2283 "yacc.tab.cpp"
    break;

  case 68: /* op: '<'  */
#line 405 "yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LT;
    }
#line 2291 "yacc.tab.cpp"
    break;

  case 69: /* op: '>'  */
#line 409 "yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GT;
    }
#line 2299 "yacc.tab.cpp"
    break;

  case 70: /* op: NEQ  */
#line 413 "yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_NE;
    }
#line 2307 "yacc.tab.cpp"
    break;

  case 71: /* op: LEQ  */
#line 417 "yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_LE;
    }
#line 2315 "yacc.tab.cpp"
    break;

  case 72: /* op: GEQ  */
#line 421 "yacc.y"
    {
        (yyval.sv_comp_op) = SV_OP_GE;
    }
#line 2323 "yacc.tab.cpp"
    break;

  case 73: /* expr: value  */
#line 428 "yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_val));
    }
#line 2331 "yacc.tab.cpp"
    break;

  case 74: /* expr: col  */
#line 432 "yacc.y"
    {
        (yyval.sv_expr) = std::static_pointer_cast<Expr>((yyvsp[0].sv_col));
    }
#line 2339 "yacc.tab.cpp"
    break;

  case 75: /* setClauses: setClause  */
#line 439 "yacc.y"
    {
        (yyval.sv_set_clauses) = std::vector<std::shared_ptr<SetClause>>{(yyvsp[0].sv_set_clause)};
    }
#line 2347 "yacc.tab.cpp"
    break;

  case 76: /* setClauses: setClauses ',' setClause  */
#line 443 "yacc.y"
    {
        (yyval.sv_set_clauses).push_back((yyvsp[0].sv_set_clause));
    }
#line 2355 "yacc.tab.cpp"
    break;

  case 77: /* setClause: colName '=' value  */
#line 450 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-2].sv_str), (yyvsp[0].sv_val));
    }
#line 2363 "yacc.tab.cpp"
    break;

  case 78: /* setClause: colName '=' colName  */
#line 454 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2371 "yacc.tab.cpp"
    break;

  case 79: /* setClause: arithmeticSetClause  */
#line 458 "yacc.y"
    {
        (yyval.sv_set_clause) = (yyvsp[0].sv_set_clause);
    }
#line 2379 "yacc.tab.cpp"
    break;

  case 80: /* arithmeticSetClause: colName '=' colName '+' value  */
#line 465 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), (yyvsp[0].sv_val), SET_OP_ADD);
    }
#line 2387 "yacc.tab.cpp"
    break;

  case 81: /* arithmeticSetClause: colName '=' colName '-' value  */
#line 469 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), (yyvsp[0].sv_val), SET_OP_SUB);
    }
#line 2395 "yacc.tab.cpp"
    break;

  case 82: /* arithmeticSetClause: colName '=' colName '*' value  */
#line 473 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), (yyvsp[0].sv_val), SET_OP_MUL);
    }
#line 2403 "yacc.tab.cpp"
    break;

  case 83: /* arithmeticSetClause: colName '=' colName '/' value  */
#line 477 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[-2].sv_str), (yyvsp[0].sv_val), SET_OP_DIV);
    }
#line 2411 "yacc.tab.cpp"
    break;

  case 84: /* arithmeticSetClause: colName '=' value '+' colName  */
#line 481 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[0].sv_str), (yyvsp[-2].sv_val), SET_OP_ADD, true);
    }
#line 2419 "yacc.tab.cpp"
    break;

  case 85: /* arithmeticSetClause: colName '=' value '-' colName  */
#line 485 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[0].sv_str), (yyvsp[-2].sv_val), SET_OP_SUB, true);
    }
#line 2427 "yacc.tab.cpp"
    break;

  case 86: /* arithmeticSetClause: colName '=' value '*' colName  */
#line 489 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[0].sv_str), (yyvsp[-2].sv_val), SET_OP_MUL, true);
    }
#line 2435 "yacc.tab.cpp"
    break;

  case 87: /* arithmeticSetClause: colName '=' value '/' colName  */
#line 493 "yacc.y"
    {
        (yyval.sv_set_clause) = std::make_shared<SetClause>((yyvsp[-4].sv_str), (yyvsp[0].sv_str), (yyvsp[-2].sv_val), SET_OP_DIV, true);
    }
#line 2443 "yacc.tab.cpp"
    break;

  case 88: /* arithmeticSetClause: arithmeticSetClause '+' value  */
#line 497 "yacc.y"
    {
        (yyvsp[-2].sv_set_clause)->append_step(SET_OP_ADD, (yyvsp[0].sv_val));
        (yyval.sv_set_clause) = (yyvsp[-2].sv_set_clause);
    }
#line 2452 "yacc.tab.cpp"
    break;

  case 89: /* arithmeticSetClause: arithmeticSetClause '-' value  */
#line 502 "yacc.y"
    {
        (yyvsp[-2].sv_set_clause)->append_step(SET_OP_SUB, (yyvsp[0].sv_val));
        (yyval.sv_set_clause) = (yyvsp[-2].sv_set_clause);
    }
#line 2461 "yacc.tab.cpp"
    break;

  case 90: /* arithmeticSetClause: arithmeticSetClause '*' value  */
#line 507 "yacc.y"
    {
        (yyvsp[-2].sv_set_clause)->append_step(SET_OP_MUL, (yyvsp[0].sv_val));
        (yyval.sv_set_clause) = (yyvsp[-2].sv_set_clause);
    }
#line 2470 "yacc.tab.cpp"
    break;

  case 91: /* arithmeticSetClause: arithmeticSetClause '/' value  */
#line 512 "yacc.y"
    {
        (yyvsp[-2].sv_set_clause)->append_step(SET_OP_DIV, (yyvsp[0].sv_val));
        (yyval.sv_set_clause) = (yyvsp[-2].sv_set_clause);
    }
#line 2479 "yacc.tab.cpp"
    break;

  case 92: /* newSelector: '*'  */
#line 528 "yacc.y"
    {
        (yyval.sv_select_items) = {};
    }
#line 2487 "yacc.tab.cpp"
    break;

  case 94: /* selectItemList: selectItem  */
#line 536 "yacc.y"
    {
        (yyval.sv_select_items) = std::vector<std::shared_ptr<SelectItem>>{(yyvsp[0].sv_select_item)};
    }
#line 2495 "yacc.tab.cpp"
    break;

  case 95: /* selectItemList: selectItemList ',' selectItem  */
#line 540 "yacc.y"
    {
        (yyval.sv_select_items).push_back((yyvsp[0].sv_select_item));
    }
#line 2503 "yacc.tab.cpp"
    break;

  case 96: /* selectItem: col  */
#line 547 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>((yyvsp[0].sv_col));
    }
#line 2511 "yacc.tab.cpp"
    break;

  case 97: /* selectItem: col AS colName  */
#line 551 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>((yyvsp[-2].sv_col), (yyvsp[0].sv_str));
    }
#line 2519 "yacc.tab.cpp"
    break;

  case 98: /* selectItem: aggregateItem  */
#line 555 "yacc.y"
    {
        (yyval.sv_select_item) = (yyvsp[0].sv_select_item);
    }
#line 2527 "yacc.tab.cpp"
    break;

  case 99: /* selectItem: aggregateItem AS colName  */
#line 559 "yacc.y"
    {
        (yyvsp[-2].sv_select_item)->alias = (yyvsp[0].sv_str);
        (yyval.sv_select_item) = (yyvsp[-2].sv_select_item);
    }
#line 2536 "yacc.tab.cpp"
    break;

  case 100: /* aggregateItem: aggName '(' col ')'  */
#line 567 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>((yyvsp[-3].sv_agg_type), (yyvsp[-1].sv_col), false);
    }
#line 2544 "yacc.tab.cpp"
    break;

  case 101: /* aggregateItem: COUNT '(' col ')'  */
#line 571 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>(AGG_COUNT, (yyvsp[-1].sv_col), false);
    }
#line 2552 "yacc.tab.cpp"
    break;

  case 102: /* aggregateItem: COUNT '(' '*' ')'  */
#line 575 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>(AGG_COUNT, nullptr, true);
    }
#line 2560 "yacc.tab.cpp"
    break;

  case 103: /* aggregateItem: COUNT '(' DISTINCT col ')'  */
#line 579 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>(AGG_COUNT, (yyvsp[-1].sv_col), false, "", true);
    }
#line 2568 "yacc.tab.cpp"
    break;

  case 104: /* aggregateItem: COUNT '(' DISTINCT '(' col ')' ')'  */
#line 583 "yacc.y"
    {
        (yyval.sv_select_item) = std::make_shared<SelectItem>(AGG_COUNT, (yyvsp[-2].sv_col), false, "", true);
    }
#line 2576 "yacc.tab.cpp"
    break;

  case 105: /* aggName: MAX  */
#line 590 "yacc.y"
    {
        (yyval.sv_agg_type) = AGG_MAX;
    }
#line 2584 "yacc.tab.cpp"
    break;

  case 106: /* aggName: MIN  */
#line 594 "yacc.y"
    {
        (yyval.sv_agg_type) = AGG_MIN;
    }
#line 2592 "yacc.tab.cpp"
    break;

  case 107: /* aggName: SUM  */
#line 598 "yacc.y"
    {
        (yyval.sv_agg_type) = AGG_SUM;
    }
#line 2600 "yacc.tab.cpp"
    break;

  case 108: /* aggName: AVG  */
#line 602 "yacc.y"
    {
        (yyval.sv_agg_type) = AGG_AVG;
    }
#line 2608 "yacc.tab.cpp"
    break;

  case 109: /* fromList: tableRef  */
#line 624 "yacc.y"
    {
        (yyval.sv_from) = std::make_shared<FromClause>();
        (yyval.sv_from)->table_refs.push_back((yyvsp[0].sv_table_ref));
    }
#line 2617 "yacc.tab.cpp"
    break;

  case 110: /* fromList: fromList ',' tableRef  */
#line 629 "yacc.y"
    {
        (yyval.sv_from) = (yyvsp[-2].sv_from);
        (yyval.sv_from)->table_refs.push_back((yyvsp[0].sv_table_ref));
    }
#line 2626 "yacc.tab.cpp"
    break;

  case 111: /* fromList: fromList JOIN tableRef optJoinOnClause  */
#line 634 "yacc.y"
    {
        (yyval.sv_from) = (yyvsp[-3].sv_from);
        (yyval.sv_from)->table_refs.push_back((yyvsp[-1].sv_table_ref));
        (yyval.sv_from)->join_conds.insert((yyval.sv_from)->join_conds.end(), (yyvsp[0].sv_join_conds).begin(), (yyvsp[0].sv_join_conds).end());
    }
#line 2636 "yacc.tab.cpp"
    break;

  case 112: /* fromList: tableRef SEMI JOIN tableRef ON condition  */
#line 640 "yacc.y"
    {
        (yyval.sv_from) = std::make_shared<FromClause>();
        (yyval.sv_from)->table_refs.push_back((yyvsp[-5].sv_table_ref));
        (yyval.sv_from)->table_refs.push_back((yyvsp[-2].sv_table_ref));
        (yyval.sv_from)->is_semi_join = true;
        (yyval.sv_from)->semi_conds.push_back((yyvsp[0].sv_cond));
    }
#line 2648 "yacc.tab.cpp"
    break;

  case 113: /* optJoinOnClause: ON whereClause  */
#line 651 "yacc.y"
    {
        (yyval.sv_join_conds) = (yyvsp[0].sv_conds);
    }
#line 2656 "yacc.tab.cpp"
    break;

  case 114: /* optJoinOnClause: %empty  */
#line 654 "yacc.y"
                      { (yyval.sv_join_conds) = {}; }
#line 2662 "yacc.tab.cpp"
    break;

  case 115: /* tableRef: tbName  */
#line 659 "yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[0].sv_str), "");
    }
#line 2670 "yacc.tab.cpp"
    break;

  case 116: /* tableRef: tbName tbName  */
#line 663 "yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-1].sv_str), (yyvsp[0].sv_str));
    }
#line 2678 "yacc.tab.cpp"
    break;

  case 117: /* tableRef: tbName AS tbName  */
#line 667 "yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-2].sv_str), (yyvsp[0].sv_str));
    }
#line 2686 "yacc.tab.cpp"
    break;

  case 118: /* tableRef: '(' unionSelectList ')' AS tbName  */
#line 671 "yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-3].sv_select_stmts), (yyvsp[0].sv_str));
    }
#line 2694 "yacc.tab.cpp"
    break;

  case 119: /* tableRef: '(' unionSelectList ')' tbName  */
#line 675 "yacc.y"
    {
        (yyval.sv_table_ref) = std::make_shared<TableRef>((yyvsp[-2].sv_select_stmts), (yyvsp[0].sv_str));
    }
#line 2702 "yacc.tab.cpp"
    break;

  case 120: /* opt_order_clause: ORDER BY order_clause  */
#line 682 "yacc.y"
    { 
        (yyval.sv_orderby) = (yyvsp[0].sv_orderby); 
    }
#line 2710 "yacc.tab.cpp"
    break;

  case 121: /* opt_order_clause: %empty  */
#line 685 "yacc.y"
                      { (yyval.sv_orderby) = nullptr; }
#line 2716 "yacc.tab.cpp"
    break;

  case 122: /* order_clause: order_item_list  */
#line 690 "yacc.y"
    { 
        (yyval.sv_orderby) = std::make_shared<OrderBy>((yyvsp[0].sv_orderby_items));
    }
#line 2724 "yacc.tab.cpp"
    break;

  case 123: /* order_item_list: order_item  */
#line 697 "yacc.y"
    {
        (yyval.sv_orderby_items) = std::vector<std::shared_ptr<OrderByItem>>{(yyvsp[0].sv_orderby_item)};
    }
#line 2732 "yacc.tab.cpp"
    break;

  case 124: /* order_item_list: order_item_list ',' order_item  */
#line 701 "yacc.y"
    {
        (yyval.sv_orderby_items).push_back((yyvsp[0].sv_orderby_item));
    }
#line 2740 "yacc.tab.cpp"
    break;

  case 125: /* order_item: col opt_asc_desc  */
#line 708 "yacc.y"
    {
        (yyval.sv_orderby_item) = std::make_shared<OrderByItem>((yyvsp[-1].sv_col), (yyvsp[0].sv_orderby_dir));
    }
#line 2748 "yacc.tab.cpp"
    break;

  case 126: /* opt_asc_desc: ASC  */
#line 714 "yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_ASC;     }
#line 2754 "yacc.tab.cpp"
    break;

  case 127: /* opt_asc_desc: DESC  */
#line 715 "yacc.y"
                 { (yyval.sv_orderby_dir) = OrderBy_DESC;    }
#line 2760 "yacc.tab.cpp"
    break;

  case 128: /* opt_asc_desc: %empty  */
#line 716 "yacc.y"
            { (yyval.sv_orderby_dir) = OrderBy_DEFAULT; }
#line 2766 "yacc.tab.cpp"
    break;

  case 129: /* optGroupClause: GROUP BY colList  */
#line 721 "yacc.y"
    {
        (yyval.sv_cols) = (yyvsp[0].sv_cols);
    }
#line 2774 "yacc.tab.cpp"
    break;

  case 130: /* optGroupClause: %empty  */
#line 724 "yacc.y"
                      { (yyval.sv_cols) = {}; }
#line 2780 "yacc.tab.cpp"
    break;

  case 131: /* optHavingClause: HAVING havingClause  */
#line 729 "yacc.y"
    {
        (yyval.sv_havings) = (yyvsp[0].sv_havings);
    }
#line 2788 "yacc.tab.cpp"
    break;

  case 132: /* optHavingClause: %empty  */
#line 732 "yacc.y"
                      { (yyval.sv_havings) = {}; }
#line 2794 "yacc.tab.cpp"
    break;

  case 133: /* havingClause: havingCondition  */
#line 737 "yacc.y"
    {
        (yyval.sv_havings) = std::vector<std::shared_ptr<HavingExpr>>{(yyvsp[0].sv_having)};
    }
#line 2802 "yacc.tab.cpp"
    break;

  case 134: /* havingClause: havingClause AND havingCondition  */
#line 741 "yacc.y"
    {
        (yyval.sv_havings).push_back((yyvsp[0].sv_having));
    }
#line 2810 "yacc.tab.cpp"
    break;

  case 135: /* havingCondition: havingLhs op value  */
#line 748 "yacc.y"
    {
        (yyval.sv_having) = std::make_shared<HavingExpr>((yyvsp[-2].sv_select_item), (yyvsp[-1].sv_comp_op), (yyvsp[0].sv_val));
    }
#line 2818 "yacc.tab.cpp"
    break;

  case 136: /* havingLhs: aggregateItem  */
#line 755 "yacc.y"
    {
        (yyval.sv_select_item) = (yyvsp[0].sv_select_item);
    }
#line 2826 "yacc.tab.cpp"
    break;

  case 137: /* optLimitClause: LIMIT VALUE_INT  */
#line 762 "yacc.y"
    {
        (yyval.sv_int) = (yyvsp[0].sv_int);
    }
#line 2834 "yacc.tab.cpp"
    break;

  case 138: /* optLimitClause: %empty  */
#line 765 "yacc.y"
                      { (yyval.sv_int) = -1; }
#line 2840 "yacc.tab.cpp"
    break;

  case 139: /* set_knob_type: ENABLE_NESTLOOP  */
#line 769 "yacc.y"
                    { (yyval.sv_setKnobType) = EnableNestLoop; }
#line 2846 "yacc.tab.cpp"
    break;

  case 140: /* set_knob_type: ENABLE_SORTMERGE  */
#line 770 "yacc.y"
                         { (yyval.sv_setKnobType) = EnableSortMerge; }
#line 2852 "yacc.tab.cpp"
    break;


#line 2856 "yacc.tab.cpp"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      {
        yypcontext_t yyctx
          = {yyssp, yytoken, &yylloc};
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == -1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *,
                             YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (yymsg)
              {
                yysyntax_error_status
                  = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
                yymsgp = yymsg;
              }
            else
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = YYENOMEM;
              }
          }
        yyerror (&yylloc, context, scanner, yymsgp);
        if (yysyntax_error_status == YYENOMEM)
          YYNOMEM;
      }
    }

  yyerror_range[1] = yylloc;
  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, &yylloc, context, scanner);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;

      yyerror_range[1] = *yylsp;
      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, yylsp, context, scanner);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  ++yylsp;
  YYLLOC_DEFAULT (*yylsp, yyerror_range, 2);

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, context, scanner, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, &yylloc, context, scanner);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, yylsp, context, scanner);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
  return yyresult;
}

#line 776 "yacc.y"

