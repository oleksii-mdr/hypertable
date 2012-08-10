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

#ifndef HYPERTABLE_HQLPARSER2_H
#define HYPERTABLE_HQLPARSER2_H

#include <boost/config/warning_disable.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/qi_no_case.hpp>
#include <boost/spirit/include/phoenix_bind.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_fusion.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>
#include <boost/spirit/include/phoenix_object.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/fusion/include/io.hpp>

#include <iostream>
#include <string>

#include <sys/types.h>

namespace Hql {

  const char *commands[] = {
    "???",
    "HELP",
    "CREATE_TABLE",
    "DESCRIBE_TABLE",
    "SHOW_CREATE_TABLE",
    "SELECT",
    "LOAD_DATA",
    "INSERT",
    "DELETE",
    "GET_LISTING",
    "DROP_TABLE",
    "ALTER_TABLE",
    "CREATE_SCANNER",
    "DESTROY_SCANNER",
    "FETCH_SCANBLOCK",
    "LOAD_RANGE",
    "SHUTDOWN",
    "SHUTDOWN_MASTER",
    "UPDATE",
    "REPLAY_BEGIN",
    "REPLAY_LOAD_RANGE",
    "REPLAY_LOG",
    "REPLAY_COMMIT",
    "DROP_RANGE",
    "DUMP",
    "CLOSE",
    "DUMP_TABLE",
    "EXISTS_TABLE",
    "USE_NAMESPACE",
    "CREATE_NAMESPACE",
    "DROP_NAMESPACE",
    "RENAME_TABLE",
    "WAIT_FOR_MAINTENANCE",
    "BALANCE",
    "HEAPCHECK",
    "COMPACT",
    "METADATA_SYNC",
    "STOP",
    (const char *)0
  };

  namespace fusion = boost::fusion;
  namespace phoenix = boost::phoenix;
  namespace qi = boost::spirit::qi;
  namespace ascii = boost::spirit::ascii;

  class split_date {
  public:
    split_date() : year(0), month(0), day(0), hour(0), minute(0), sec(0), nsec(0) { }
    void to_string() { std::cout << year << "-" << month << "-" << day << " " << hour << ":" << minute << ":" << sec << ":" << nsec << std::endl; }
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int sec;
    int nsec;
  };
  std::ostream &operator<<(std::ostream &os, const split_date &sd) {
    os << sd.year << "-" << sd.month << "-" << sd.day << " " << sd.hour << ":" << sd.minute << ":" << sd.sec << ":" << sd.nsec;
    return os;
  }

  typedef boost::variant< ::int64_t, split_date> date_variant;

  class date_variant_visitor  : public boost::static_visitor<> {
  public:
    date_variant_visitor(std::ostream &os) : out(os) { }
    void operator()(const split_date &sd) const {
      out << sd;
    }
    void operator()(const ::int64_t &i) const {
      out << i;
    }
    std::ostream &out;
  };
  std::ostream &operator<<(std::ostream &os, const date_variant &dv) {
    boost::apply_visitor( date_variant_visitor(os), dv );
    return os;
  }

  class Value {
  public:
    Value() : type(0), timestamp(INT64_MIN+2), revision(INT64_MIN+2) { }
    int type;
    std::string row;
    std::string column;
    std::string value;
    date_variant timestamp;
    date_variant revision;
  };

  std::ostream &operator<<(std::ostream &os, const Value &v) {
    os << "(";
    if (v.type == 0)
      os << "INS,";
    else if (v.type == 1)
      os << "DEL,";
    else if (v.type == 2)
      os << "DEL_VER,";
    else
      os << "UNKNOWN,";
    os << v.row << ",";
    if (!v.column.empty()) {
      os << v.column << ",";
      if (!v.value.empty())
        os << v.value << ",";
    }
    os << v.timestamp << "," << v.revision << ")";
    return os;
  }

  class ParserState {
  public:
    ParserState() : command(0) { }
    void set_command(int c) { command=c; }
    int command;
    std::string table_name;
    std::vector<Value> values;
  };

}

BOOST_FUSION_ADAPT_STRUCT(
                          Hql::split_date,
                          (int, year)
                          (int, month)
                          (int, day)
                          (int, hour)
                          (int, minute)
                          (int, sec)
                          (int, nsec)
                          )

BOOST_FUSION_ADAPT_STRUCT(
                          Hql::Value,
                          (int, type)
                          (std::string, row)
                          (std::string, column)
                          (std::string, value)
                          (Hql::date_variant, timestamp)
                          (Hql::date_variant, revision)
                          )

namespace Hql {

  using qi::symbols;

  template <typename Iterator>
  struct hql_parser : qi::grammar<Iterator, ParserState(), qi::locals<std::string>, ascii::space_type>
  {

    symbols<char, ::int64_t> date_symbols;
    symbols<char, int> command_symbols;
    
    hql_parser() : hql_parser::base_type(statement, "statement")
    {
      using qi::alpha;
      using qi::alnum;
      using qi::int_;
      using qi::lit;
      using qi::double_;
      using qi::long_long;
      using qi::lexeme;
      using qi::no_case;
      using qi::on_error;
      using qi::fail;
      using ascii::char_;
      using ascii::string;
      using namespace qi::labels;

      using phoenix::construct;
      using phoenix::val;
      using phoenix::at_c;
      using phoenix::bind;
      using phoenix::push_back;
      using phoenix::ref;

      date_symbols.add
        ("auto", INT64_MIN + 2)
        ("null", INT64_MIN + 1)
        ("min", INT64_MIN)
        ("max", INT64_MAX);

      for (int i=1; commands[i]; i++)
        command_symbols.add(commands[i], i);

      statement =
        statement_insert.alias()
        ;

      statement_insert =
        no_case[lit("insert")] [bind(&ParserState::command, _val) = command_symbols.at("INSERT")]
        > no_case[lit("into")]
        > user_identifier [bind(&ParserState::table_name, _val) = _1]
        > no_case[lit("values")]
        > +( value [push_back(bind(&ParserState::values, _val), _1)] )
        ;

      user_identifier =
        identifier
        | quoted_string
        ;

      identifier %= lexeme[alpha >> *(alnum | '_')];

      quoted_string =
        double_quoted_string
        | single_quoted_string
        ;

      double_quoted_string %= lexeme['"' >> +('\\' >> char_("\"") | ~char_('"')) >> '"'];

      single_quoted_string %= lexeme['\'' >> +('\\' >> char_("'") | ~char_('\'')) >> '\''];

      value = '(' >>
        ( insert
          | delete_row
          | delete_column
          | delete_cell_version
          ) >> ')'
        ;

      insert =
        -(no_case[lit("ins")] >> ',')
        >> quoted_string [at_c<1>(_val) = _1] > ','
        > user_identifier [at_c<2>(_val) = _1] > ','
        > quoted_string [at_c<3>(_val) = _1]
        >> -( ',' >> date [at_c<4>(_val) = _1]
              >> -( ',' >> date [at_c<5>(_val) = _1]))
        ;

      delete_row =
        no_case[lit("del")] [at_c<0>(_val) = 1] >> ','
        > quoted_string [at_c<1>(_val) = _1]
        >> -( ',' >> date [at_c<4>(_val) = _1]
              >> -( ',' >> date [at_c<5>(_val) = _1]))
        ;

      /* no_case[lit("del") >> ~char_('_')] [at_c<0>(_val) = 1]  >> ','*/

      delete_column =
        no_case[lit("del")] [at_c<0>(_val) = 1] >> ','
        > quoted_string [at_c<1>(_val) = _1] > ','
        > user_identifier [at_c<2>(_val) = _1]
        >> -( ',' >> date [at_c<4>(_val) = _1]
              >> -( ',' >> date [at_c<5>(_val) = _1]))
        ;

      delete_cell_version =
        no_case[lit("del_ver")] [at_c<0>(_val) = 2] 
        > ',' > quoted_string [at_c<1>(_val) = _1] 
        > ',' > user_identifier [at_c<2>(_val) = _1]
        > ',' > date [at_c<4>(_val) = _1]
        >> -( ',' >> date [at_c<5>(_val) = _1])
        ;

      date = 
        ( date_formatted [_val = _1]
          || time_since_epoch [_val = _1]
          || date_symbols  [_val = _1])
        ;

      date_formatted =
        int_ [at_c<0>(_val) = _1] >> !(lit('s') || lit("ns"))
        >> -('-' >> int_ [at_c<1>(_val) = _1]
             >> -( '-' >> int_ [at_c<2>(_val) = _1]
                   >> -( int_ [at_c<3>(_val) = _1]
                         > ':' > int_ [at_c<4>(_val) = _1]
                         > ':' > int_ [at_c<5>(_val) = _1]
                         >> -(':' >> int_ [at_c<6>(_val) = _1]))))
        ;

      time_since_epoch =
        ( long_long[_val = _1] >> "ns" )
        || ( long_long [_val = _1*1000000000LL] >> 's' )
        ;


      statement.name("statement");
      statement_insert.name("statement_insert");
      identifier.name("identifier");
      user_identifier.name("user_identifier");
      quoted_string.name("quoted_string");
      double_quoted_string.name("double_quoted_string");
      single_quoted_string.name("single_quoted_string");
      value.name("value");
      delete_row.name("delete_row");
      delete_column.name("delete_column");
      delete_cell_version.name("delete_cell_version");
      insert.name("insert");
      date.name("date");
      date_formatted.name("date_formatted");
      time_since_epoch.name("time_since_epoch");

      on_error<fail>
        (
         statement
         , std::cout
         << val("Error! Expecting ")
         << _4                               // what failed?
         << val(" here: \"")
         << construct<std::string>(_3, _2)   // iterators to error-pos, end
         << val("\"")
         << std::endl
         );      
    }

    qi::rule<Iterator, ParserState(), qi::locals<std::string>, ascii::space_type> statement;
    qi::rule<Iterator, ParserState(), ascii::space_type> statement_insert;
    qi::rule<Iterator, std::string(), ascii::space_type> identifier;
    qi::rule<Iterator, std::string(), ascii::space_type> user_identifier;
    qi::rule<Iterator, std::string(), ascii::space_type> quoted_string;
    qi::rule<Iterator, std::string(), ascii::space_type> double_quoted_string;
    qi::rule<Iterator, std::string(), ascii::space_type> single_quoted_string;
    qi::rule<Iterator, Value(), ascii::space_type> value;
    qi::rule<Iterator, Value(), ascii::space_type> insert;
    qi::rule<Iterator, Value(), ascii::space_type> delete_row;
    qi::rule<Iterator, Value(), ascii::space_type> delete_column;
    qi::rule<Iterator, Value(), ascii::space_type> delete_cell_version;
    qi::rule<Iterator, date_variant(), ascii::space_type> date;
    qi::rule<Iterator, split_date(), ascii::space_type> date_formatted;
    qi::rule<Iterator, ::int64_t(), ascii::space_type> time_since_epoch;
  };
}


#endif // HYPERTABLE_HQLPARSER2_H
