/** -*- c++ -*-
 * Copyright (C) 2007-2012 Hypertable, Inc.
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 3 of the
 * License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "Common/Compat.h"

#include "Hypertable/Lib/HqlParser2.h"

int main() {
  std::cout << "/////////////////////////////////////////////////////////\n\n";
  std::cout << "\t\tAn employee parser for Spirit...\n\n";
  std::cout << "/////////////////////////////////////////////////////////\n\n";

  std::cout
    << "Give me a date of the form :"
    << "date YYYY-MM-DD HH:MM:SS:NSECS \n";
  std::cout << "Type [q or Q] to quit\n\n";

  using boost::spirit::ascii::space;
  typedef std::string::const_iterator iterator_type;
  typedef Hql::hql_parser<iterator_type> hql_parser;

  hql_parser g; // Our grammar
  std::string str;
  while (getline(std::cin, str)) {
      if (str.empty() || str[0] == 'q' || str[0] == 'Q')
        break;

      Hql::ParserState state;
      std::string::const_iterator iter = str.begin();
      std::string::const_iterator end = str.end();
      bool r = phrase_parse(iter, end, g, space, state);

      if (r && iter == end)
        {
          std::cout << "-------------------------\n";
          std::cout << "Parsing succeeded\n";
          std::cout << "Command: " << state.command << std::endl;
          for (size_t i=0; i<state.values.size(); i++)
            std::cout << state.values[i] << std::endl;
          std::cout << "\n-------------------------\n";
        }
      else
        {
          std::cout << "-------------------------\n";
          std::cout << "Parsing failed: " << *iter << std::endl;
          std::cout << "-------------------------\n";
        }
    }

  std::cout << "Bye... :-) \n\n";
  return 0;
}
