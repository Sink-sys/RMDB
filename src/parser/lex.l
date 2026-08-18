    /* keywords are case insensitive */
%option caseless
    /* we don't need yywrap() function */
%option noyywrap
    /* we don't need yyunput() function */
%option nounput
    /* we don't need input() function */
%option noinput
    /* enable location */
%option bison-bridge
%option bison-locations
    /* one scanner instance per parse */
%option reentrant

%top{
#define YY_TYPEDEF_YY_SIZE_T
typedef int yy_size_t;
}

%{
#include "ast.h"
#include "yacc.tab.h"
#include <climits>
#include <cstdlib>
#include <iostream>

// automatically update location
#define YY_USER_ACTION \
    yylloc->first_line = yylloc->last_line; \
    yylloc->first_column = yylloc->last_column; \
    for (int i = 0; yytext[i] != '\0'; i++) { \
        if(yytext[i] == '\n') { \
            yylloc->last_line++; \
            yylloc->last_column = 1; \
        } else { \
            yylloc->last_column++; \
        } \
    }

%}

alpha [a-zA-Z]
digit [0-9]
white_space [ \t]+
new_line "\r"|"\n"|"\r\n"
identifier {alpha}(_|{alpha}|{digit})*
value_int {digit}+
value_float (({digit}+\.{digit}*)|(\.{digit}+))([eE][+-]?{digit}+)?|{digit}+[eE][+-]?{digit}+
value_string '[^']*'
parameter \${digit}+
single_op ";"|"("|")"|","|"*"|"/"|"="|">"|"<"|"."|"+"|"-"

%x STATE_COMMENT
%x STATE_LOAD

%%
    /* block comment */
"/*" { BEGIN(STATE_COMMENT); }
<STATE_COMMENT>"*/" { BEGIN(INITIAL); }
<STATE_COMMENT>[^*] { /* ignore the text of the comment */ }
<STATE_COMMENT>\* { /* ignore *'s that aren't part of */ }
    /* single line comment */
"--".* { /* ignore single line comment */ }
    /* white space and new line */
{white_space} { /* ignore white space */ }
{new_line} { /* ignore new line */ }
    /* keywords */
"SHOW" { return SHOW; }
"LOAD" { BEGIN(STATE_LOAD); return LOAD; }
<STATE_LOAD>{white_space} { /* skip whitespace before load path */ }
<STATE_LOAD>[^ \t\r\n;]+ {
    yylval->sv_str = yytext;
    BEGIN(INITIAL);
    return VALUE_STRING;
}
"BEGIN" { return TXN_BEGIN; }
"COMMIT" { return TXN_COMMIT; }
"ABORT" { return TXN_ABORT; }
"ROLLBACK" { return TXN_ROLLBACK; }
"TABLES" { return TABLES; }
"CREATE" { return CREATE; }
"TABLE" { return TABLE; }
"DROP" { return DROP; }
"DESC" { return DESC; }
"INSERT" { return INSERT; }
"INTO" { return INTO; }
"VALUES" { return VALUES; }
"DELETE" { return DELETE; }
"FROM" { return FROM; }
"WHERE" { return WHERE; }
"UPDATE" { return UPDATE; }
"SET" { return SET; }
"TRANSACTION" { return TRANSACTION; }
"ISOLATION" { return ISOLATION; }
"LEVEL" { return LEVEL; }
"SNAPSHOT" { return SNAPSHOT; }
"SERIALIZABLE" { return SERIALIZABLE; }
"SELECT" { return SELECT; }
"INT" { return INT; }
"CHAR" { return CHAR; }
"FLOAT" { return FLOAT; }
"DATETIME" { return DATETIME; }
"INDEX" { return INDEX; }
"AND" { return AND; }
"JOIN" {return JOIN;}
"SEMI" { return SEMI; }
"ON" { return ON; }
"GROUP" { return GROUP; }
"HAVING" { return HAVING; }
"LIMIT" { return LIMIT; }
"AS" { return AS; }
"EXPLAIN" { return EXPLAIN; }
"ANALYZE" { return ANALYZE; }
"UNION" { return UNION; }
"MAX" { return MAX; }
"MIN" { return MIN; }
"COUNT" { return COUNT; }
"DISTINCT" { return DISTINCT; }
"SUM" { return SUM; }
"AVG" { return AVG; }
"EXIT" { return EXIT; }
"HELP" { return HELP; }
"ORDER" { return ORDER; }
"BY" {  return BY;  }
"ASC" { return ASC; }
"ENABLE_NESTLOOP" { return ENABLE_NESTLOOP; }
"ENABLE_SORTMERGE" { return ENABLE_SORTMERGE; }
"STATIC_CHECKPOINT" { return STATIC_CHECKPOINT; }
"TRUE" { 
    yylval->sv_bool = true;
    return VALUE_BOOL; 
}
"FALSE" {
    yylval->sv_bool = false;
    return VALUE_BOOL;
}
    /* operators */
">=" { return GEQ; }
"<=" { return LEQ; }
"<>" { return NEQ; }
{single_op} { return yytext[0]; }
    /* id */
{identifier} {
    yylval->sv_str = yytext;
    return IDENTIFIER;
}
    /* literals */
{parameter} {
    unsigned long long ordinal = strtoull(yytext + 1, nullptr, 10);
    yylval->sv_int = ordinal > INT_MAX ? INT_MAX : static_cast<int>(ordinal);
    return PARAMETER;
}
{value_int} {
    yylval->sv_int = atoi(yytext);
    return VALUE_INT;
}
{value_float} {
    yylval->sv_float = strtof(yytext, nullptr);
    return VALUE_FLOAT;
}
{value_string} {
    yylval->sv_str = std::string(yytext + 1, strlen(yytext) - 2);
    return VALUE_STRING;
}
"!=" { return NEQ; }
    /* EOF */
<<EOF>> { return T_EOF; }
    /* unexpected char */
. { std::cerr << "Lexer Error: unexpected character " << yytext[0] << std::endl; }
%%
