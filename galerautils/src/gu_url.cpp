
#include "gu_url.hpp"
#include "gu_logger.hpp"
#include "gu_string.hpp"

#include <stdexcept>
#include <vector>

#include <sys/types.h>
#include <regex.h>

using std::string;
using std::vector;
using std::multimap;

static void logerr(int rc, const regex_t* preg, const string& msg = "")
{
    char buf[128];
    regerror(rc, preg, buf, sizeof(buf));
    log_error << buf << " (" << msg << ")";
}


static string extract_str(const string& s, const regmatch_t& rm)
{
    if (rm.rm_so == -1)
    {
        return "";
    }
    return s.substr(rm.rm_so, rm.rm_eo - rm.rm_so);
}

static gu::URLQueryList extract_query_list(const string& s,
                                           const regmatch_t& rm)
{
    string query = extract_str(s, rm);    
    gu::URLQueryList ret;
    // scan all key=value pairs
    vector<string> qlist = gu::strsplit(query, '&');
    for (vector<string>::iterator i = qlist.begin(); i != qlist.end(); ++i)
    {
        vector<string> kvlist = gu::strsplit(*i, '=');
        if (kvlist.size() != 2)
        {
            log_error << "invalid url query part: '" << query << "'";
            throw std::invalid_argument("invalid url query part");
        }
        ret.insert(make_pair(kvlist[0], kvlist[1]));
    }
    return ret;
}


gu::URL::URL(const string& str_) :
    str(str_),
    scheme(),
    authority(),
    path(),
    query_list()
{
    parse();
}

void gu::URL::parse()
{
    const char* const uri_regex = "^(([^:/?#]+):)?(//([^/?#]*))?([^?#]*)(\\?([^#]*))?(#(.*))?";
    regex_t reg;
    int rc;
    
    log_debug << "URL: " << str;
    
    if ((rc = regcomp(&reg, uri_regex, REG_EXTENDED)) != 0)
    {
        logerr(rc, &reg);
        throw std::logic_error("invalid regex");
    }
    
    /* Possibly 9 matches + beginning of line */
    const size_t nmatch = 10;
    regmatch_t pmatch[nmatch];
    
    if ((rc = regexec(&reg, str.c_str(), nmatch, pmatch, 0)) != 0)
    {
        logerr(rc, &reg, str);
        regfree(&reg);
        throw std::invalid_argument("invalid url");
    }
    regfree(&reg);    
    
    scheme = extract_str(str, pmatch[2]);
    authority = extract_str(str, pmatch[4]);
    path = extract_str(str, pmatch[5]);
    query_list = extract_query_list(str, pmatch[7]);

    log_debug << scheme << "://" << authority;
}

const string& gu::URL::get_scheme() const
{
    return scheme;
}

const string& gu::URL::get_authority() const
{
    return authority;
}

const string& gu::URL::get_path() const
{
    return path;
}

const gu::URLQueryList& gu::URL::get_query_list() const
{
    return query_list;
}