/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

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

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     BUILDDIR = 258,
     COMMA = 259,
     EQUALS = 260,
     INIT = 261,
     MACHINE = 262,
     OBJECTDIR = 263,
     OPTIONS = 264,
     MAKEOPTIONS = 265,
     PSEUDO_DEVICE = 266,
     SEMICOLON = 267,
     SOURCEDIR = 268,
     TRACE = 269,
     ID = 270,
     NUMBER = 271
   };
#endif
/* Tokens.  */
#define BUILDDIR 258
#define COMMA 259
#define EQUALS 260
#define INIT 261
#define MACHINE 262
#define OBJECTDIR 263
#define OPTIONS 264
#define MAKEOPTIONS 265
#define PSEUDO_DEVICE 266
#define SEMICOLON 267
#define SOURCEDIR 268
#define TRACE 269
#define ID 270
#define NUMBER 271




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 30 "/Users/runner/work/ios-on7xelte/ios-on7xelte/dxb/xnu/SETUP/config/parser.y"
{
	char	*str;
	int	val;
	struct	file_list *file;
	struct	idlst *lst;
}
/* Line 1529 of yacc.c.  */
#line 88 "parser.h"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;

