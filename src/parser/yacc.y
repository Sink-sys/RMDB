%{
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
%}

%code requires {
#include "parser_context.h"
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif
}

// request a pure (reentrant) parser
%define api.pure full
%parse-param { rmdb::ParserContext *context }
%parse-param { yyscan_t scanner }
%lex-param { yyscan_t scanner }
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY
WHERE UPDATE SET TRANSACTION ISOLATION LEVEL SNAPSHOT SERIALIZABLE SELECT INT CHAR FLOAT DATETIME INDEX AND JOIN SEMI ON GROUP HAVING LIMIT AS EXPLAIN ANALYZE UNION EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY ENABLE_NESTLOOP ENABLE_SORTMERGE STATIC_CHECKPOINT LOAD
%token MAX MIN COUNT SUM AVG DISTINCT
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_int> VALUE_INT PARAMETER
%token <sv_float> VALUE_FLOAT
%token <sv_bool> VALUE_BOOL

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt setStmt selectStmt
%type <sv_select_stmt> plainSelectStmt unionSelect
%type <sv_select_stmts> unionSelectList
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName
%type <sv_strs> tableList colNameList
%type <sv_col> col
%type <sv_cols> colList selector
%type <sv_select_item> selectItem aggregateItem havingLhs
%type <sv_select_items> selectItemList newSelector
%type <sv_table_ref> tableRef
%type <sv_from> fromList
%type <sv_join_conds> optJoinOnClause
%type <sv_having> havingCondition
%type <sv_havings> havingClause optHavingClause
%type <sv_cols> optGroupClause
%type <sv_set_clause> setClause arithmeticSetClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause
%type <sv_orderby>  order_clause opt_order_clause
%type <sv_orderby_item> order_item
%type <sv_orderby_items> order_item_list
%type <sv_orderby_dir> opt_asc_desc
%type <sv_agg_type> aggName
%type <sv_int> optLimitClause
%type <sv_setKnobType> set_knob_type

%%
start:
        stmt ';'
    {
        context->result = $1;
        YYACCEPT;
    }
    |   HELP
    {
        context->result = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        context->result.reset();
        YYACCEPT;
    }
    |   T_EOF
    {
        context->result.reset();
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    |   setStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    |   SHOW INDEX FROM tbName
    {
        $$ = std::make_shared<ShowIndex>($4);
    }
    ;

setStmt:
        SET set_knob_type '=' VALUE_BOOL
    {
        $$ = std::make_shared<SetStmt>($2, $4);
    }
    |   SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION
    {
        $$ = std::make_shared<SetTransactionIsolation>(false);
    }
    |   SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
    {
        $$ = std::make_shared<SetTransactionIsolation>(true);
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    |   CREATE STATIC_CHECKPOINT
    {
        $$ = std::make_shared<CreateCheckpoint>();
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   LOAD VALUE_STRING INTO tbName
    {
        $$ = std::make_shared<LoadStmt>($2, $4);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   selectStmt
    {
        $$ = $1;
    }
    ;

selectStmt:
        plainSelectStmt
    {
        $$ = $1;
    }
    |   EXPLAIN plainSelectStmt
    {
        $$ = std::make_shared<ExplainStmt>($2);
    }
    |   EXPLAIN ANALYZE plainSelectStmt
    {
        $$ = std::make_shared<ExplainStmt>($3);
    }
    ;

plainSelectStmt:
        SELECT newSelector FROM fromList optWhereClause optGroupClause optHavingClause opt_order_clause optLimitClause
    {
        $5.insert($5.end(), $4->join_conds.begin(), $4->join_conds.end());
        $$ = std::make_shared<SelectStmt>($2, $4->table_refs, $5, $6, $7, $8, $9, $4->is_semi_join, $4->semi_conds);
    }
    ;

unionSelectList:
        unionSelect
    {
        $$ = std::vector<std::shared_ptr<SelectStmt>>{$1};
    }
    |   unionSelectList UNION unionSelect
    {
        $1.push_back($3);
        $$ = $1;
    }
    ;

unionSelect:
        plainSelectStmt
    {
        $$ = $1;
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, $3);
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    |   DATETIME
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, 19);
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   '+' VALUE_INT
    {
        $$ = std::make_shared<IntLit>($2);
    }
    |   '-' VALUE_INT
    {
        $$ = std::make_shared<IntLit>(-$2);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   '+' VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($2);
    }
    |   '-' VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>(-$2);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    |   VALUE_BOOL
    {
        $$ = std::make_shared<BoolLit>($1);
    }
    |   PARAMETER
    {
        $$ = std::make_shared<ParameterRef>(static_cast<uint32_t>($1));
    }
    ;

condition:
        col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { $$ = {}; }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

colList:
        col
    {
        $$ = std::vector<std::shared_ptr<Col>>{$1};
    }
    |   colList ',' col
    {
        $$.push_back($3);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    |   colName '=' colName
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    |   arithmeticSetClause
    {
        $$ = $1;
    }
    ;

arithmeticSetClause:
        colName '=' colName '+' value
    {
        $$ = std::make_shared<SetClause>($1, $3, $5, SET_OP_ADD);
    }
    |   colName '=' colName '-' value
    {
        $$ = std::make_shared<SetClause>($1, $3, $5, SET_OP_SUB);
    }
    |   colName '=' colName '*' value
    {
        $$ = std::make_shared<SetClause>($1, $3, $5, SET_OP_MUL);
    }
    |   colName '=' colName '/' value
    {
        $$ = std::make_shared<SetClause>($1, $3, $5, SET_OP_DIV);
    }
    |   colName '=' value '+' colName
    {
        $$ = std::make_shared<SetClause>($1, $5, $3, SET_OP_ADD, true);
    }
    |   colName '=' value '-' colName
    {
        $$ = std::make_shared<SetClause>($1, $5, $3, SET_OP_SUB, true);
    }
    |   colName '=' value '*' colName
    {
        $$ = std::make_shared<SetClause>($1, $5, $3, SET_OP_MUL, true);
    }
    |   colName '=' value '/' colName
    {
        $$ = std::make_shared<SetClause>($1, $5, $3, SET_OP_DIV, true);
    }
    |   arithmeticSetClause '+' value
    {
        $1->append_step(SET_OP_ADD, $3);
        $$ = $1;
    }
    |   arithmeticSetClause '-' value
    {
        $1->append_step(SET_OP_SUB, $3);
        $$ = $1;
    }
    |   arithmeticSetClause '*' value
    {
        $1->append_step(SET_OP_MUL, $3);
        $$ = $1;
    }
    |   arithmeticSetClause '/' value
    {
        $1->append_step(SET_OP_DIV, $3);
        $$ = $1;
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   colList
    ;

newSelector:
        '*'
    {
        $$ = {};
    }
    |   selectItemList
    ;

selectItemList:
        selectItem
    {
        $$ = std::vector<std::shared_ptr<SelectItem>>{$1};
    }
    |   selectItemList ',' selectItem
    {
        $$.push_back($3);
    }
    ;

selectItem:
        col
    {
        $$ = std::make_shared<SelectItem>($1);
    }
    |   col AS colName
    {
        $$ = std::make_shared<SelectItem>($1, $3);
    }
    |   aggregateItem
    {
        $$ = $1;
    }
    |   aggregateItem AS colName
    {
        $1->alias = $3;
        $$ = $1;
    }
    ;

aggregateItem:
        aggName '(' col ')'
    {
        $$ = std::make_shared<SelectItem>($1, $3, false);
    }
    |   COUNT '(' col ')'
    {
        $$ = std::make_shared<SelectItem>(AGG_COUNT, $3, false);
    }
    |   COUNT '(' '*' ')'
    {
        $$ = std::make_shared<SelectItem>(AGG_COUNT, nullptr, true);
    }
    |   COUNT '(' DISTINCT col ')'
    {
        $$ = std::make_shared<SelectItem>(AGG_COUNT, $4, false, "", true);
    }
    |   COUNT '(' DISTINCT '(' col ')' ')'
    {
        $$ = std::make_shared<SelectItem>(AGG_COUNT, $5, false, "", true);
    }
    ;

aggName:
        MAX
    {
        $$ = AGG_MAX;
    }
    |   MIN
    {
        $$ = AGG_MIN;
    }
    |   SUM
    {
        $$ = AGG_SUM;
    }
    |   AVG
    {
        $$ = AGG_AVG;
    }
    ;

tableList:
        tbName
    {
        $$ = std::vector<std::string>{$1};
    }
    |   tableList ',' tbName
    {
        $$.push_back($3);
    }
    |   tableList JOIN tbName
    {
        $$.push_back($3);
    }
    ;

fromList:
        tableRef
    {
        $$ = std::make_shared<FromClause>();
        $$->table_refs.push_back($1);
    }
    |   fromList ',' tableRef
    {
        $$ = $1;
        $$->table_refs.push_back($3);
    }
    |   fromList JOIN tableRef optJoinOnClause
    {
        $$ = $1;
        $$->table_refs.push_back($3);
        $$->join_conds.insert($$->join_conds.end(), $4.begin(), $4.end());
    }
    |   tableRef SEMI JOIN tableRef ON condition
    {
        $$ = std::make_shared<FromClause>();
        $$->table_refs.push_back($1);
        $$->table_refs.push_back($4);
        $$->is_semi_join = true;
        $$->semi_conds.push_back($6);
    }
    ;

optJoinOnClause:
        ON whereClause
    {
        $$ = $2;
    }
    |   /* epsilon */ { $$ = {}; }
    ;

tableRef:
        tbName
    {
        $$ = std::make_shared<TableRef>($1, "");
    }
    |   tbName tbName
    {
        $$ = std::make_shared<TableRef>($1, $2);
    }
    |   tbName AS tbName
    {
        $$ = std::make_shared<TableRef>($1, $3);
    }
    |   '(' unionSelectList ')' AS tbName
    {
        $$ = std::make_shared<TableRef>($2, $5);
    }
    |   '(' unionSelectList ')' tbName
    {
        $$ = std::make_shared<TableRef>($2, $4);
    }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    |   /* epsilon */ { $$ = nullptr; }
    ;

order_clause:
      order_item_list
    { 
        $$ = std::make_shared<OrderBy>($1);
    }
    ;

order_item_list:
      order_item
    {
        $$ = std::vector<std::shared_ptr<OrderByItem>>{$1};
    }
    | order_item_list ',' order_item
    {
        $$.push_back($3);
    }
    ;

order_item:
      col opt_asc_desc
    {
        $$ = std::make_shared<OrderByItem>($1, $2);
    }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;    

optGroupClause:
    GROUP BY colList
    {
        $$ = $3;
    }
    |   /* epsilon */ { $$ = {}; }
    ;

optHavingClause:
    HAVING havingClause
    {
        $$ = $2;
    }
    |   /* epsilon */ { $$ = {}; }
    ;

havingClause:
    havingCondition
    {
        $$ = std::vector<std::shared_ptr<HavingExpr>>{$1};
    }
    | havingClause AND havingCondition
    {
        $$.push_back($3);
    }
    ;

havingCondition:
    havingLhs op value
    {
        $$ = std::make_shared<HavingExpr>($1, $2, $3);
    }
    ;

havingLhs:
    aggregateItem
    {
        $$ = $1;
    }
    ;

optLimitClause:
    LIMIT VALUE_INT
    {
        $$ = $2;
    }
    |   /* epsilon */ { $$ = -1; }
    ;

set_knob_type:
    ENABLE_NESTLOOP { $$ = EnableNestLoop; }
    |   ENABLE_SORTMERGE { $$ = EnableSortMerge; }
    ;

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
