/******************************************************************************
 * Copyright (c) 2014-2016 Leandro T. C. Melo (ltcmelo@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *****************************************************************************/

/*--------------------------*/
/*--- The UaiSo! Project ---*/
/*--------------------------*/

#include "D/DFactory.h"
#include "D/DAstLocator.h"
#include "D/DIncrementalLexer.h"
#include "D/DSanitizer.h"
#include "D/DLang.h"
#include "D/DTypeSystem.h"
#include "D/DUnit.h"
#include "Parsing/Lexer.h"
#include "Parsing/Parser.h"
#include "Semantic/Builtin.h"

using namespace uaiso;

std::unique_ptr<Unit> DFactory::makeUnit()
{
    return std::unique_ptr<Unit>(new DUnit);
}

std::unique_ptr<Builtin> DFactory::makeBuiltin()
{
    return std::unique_ptr<Builtin>(new Builtin);
}

LangId DFactory::langName() const
{
    return LangId::D;
}

std::unique_ptr<AstLocator> DFactory::makeAstLocator()
{
    return std::unique_ptr<AstLocator>(new DAstLocator);
}

std::unique_ptr<IncrementalLexer> DFactory::makeIncrementalLexer()
{
    return std::unique_ptr<IncrementalLexer>(new DIncrementalLexer);
}

std::unique_ptr<Sanitizer> DFactory::makeSanitizer()
{
    return std::unique_ptr<Sanitizer>(new DSanitizer);
}

std::unique_ptr<TypeSystem> DFactory::makeTypeSystem()
{
    return std::unique_ptr<TypeSystem>(new DTypeSystem);
}

std::unique_ptr<Lang> DFactory::makeLang()
{
    return std::unique_ptr<Lang>(new DLang);
}

std::unique_ptr<Lexer> DFactory::makeLexer()
{
    return std::unique_ptr<Lexer>(); // Wrap flex's lexer?
}

std::unique_ptr<Parser> DFactory::makeParser()
{
    return std::unique_ptr<Parser>(); // Wrap bison's parser?
}
