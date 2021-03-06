/*
 *      Copyright (C) 2005-2015 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Kodi; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "Scraper.h"
#include "filesystem/CurlFile.h"
#include "filesystem/File.h"
#include "filesystem/Directory.h"
#include "filesystem/PluginDirectory.h"
#include "guilib/LocalizeStrings.h"
#include "AddonManager.h"
#include "utils/ScraperParser.h"
#include "utils/ScraperUrl.h"
#include "utils/CharsetConverter.h"
#include "utils/log.h"
#include "music/infoscanner/MusicAlbumInfo.h"
#include "music/infoscanner/MusicArtistInfo.h"
#include "utils/fstrcmp.h"
#include "settings/AdvancedSettings.h"
#include "FileItem.h"
#include "utils/URIUtils.h"
#include "utils/XMLUtils.h"
#include "utils/StringUtils.h"
#include "music/MusicDatabase.h"
#include "video/VideoDatabase.h"
#include "music/Album.h"
#include "music/Artist.h"
#include "Util.h"
#include "URL.h"

#include <sstream>
#include <algorithm>

using namespace XFILE;
using namespace MUSIC_GRABBER;
using namespace VIDEO;

namespace ADDON
{

typedef struct
{
  const char*  name;
  CONTENT_TYPE type;
  int          pretty;
} ContentMapping;

static const ContentMapping content[] =
  {{"unknown",       CONTENT_NONE,          231 },
   {"albums",        CONTENT_ALBUMS,        132 },
   {"music",         CONTENT_ALBUMS,        132 },
   {"artists",       CONTENT_ARTISTS,       133 },
   {"movies",        CONTENT_MOVIES,      20342 },
   {"tvshows",       CONTENT_TVSHOWS,     20343 },
   {"musicvideos",   CONTENT_MUSICVIDEOS, 20389 }};

std::string TranslateContent(const CONTENT_TYPE &type, bool pretty/*=false*/)
{
  for (unsigned int index=0; index < ARRAY_SIZE(content); ++index)
  {
    const ContentMapping &map = content[index];
    if (type == map.type)
    {
      if (pretty && map.pretty)
        return g_localizeStrings.Get(map.pretty);
      else
        return map.name;
    }
  }
  return "";
}

CONTENT_TYPE TranslateContent(const std::string &string)
{
  for (unsigned int index=0; index < ARRAY_SIZE(content); ++index)
  {
    const ContentMapping &map = content[index];
    if (string == map.name)
      return map.type;
  }
  return CONTENT_NONE;
}

TYPE ScraperTypeFromContent(const CONTENT_TYPE &content)
{
  switch (content)
  {
  case CONTENT_ALBUMS:
    return ADDON_SCRAPER_ALBUMS;
  case CONTENT_ARTISTS:
    return ADDON_SCRAPER_ARTISTS;
  case CONTENT_MOVIES:
    return ADDON_SCRAPER_MOVIES;
  case CONTENT_MUSICVIDEOS:
    return ADDON_SCRAPER_MUSICVIDEOS;
  case CONTENT_TVSHOWS:
    return ADDON_SCRAPER_TVSHOWS;
  default:
    return ADDON_UNKNOWN;
  }
}

// if the XML root is <error>, throw CScraperError with enclosed <title>/<message> values
static void CheckScraperError(const TiXmlElement *pxeRoot)
{
  if (!pxeRoot || stricmp(pxeRoot->Value(), "error"))
    return;
  std::string sTitle;
  std::string sMessage;
  XMLUtils::GetString(pxeRoot, "title", sTitle);
  XMLUtils::GetString(pxeRoot, "message", sMessage);
  throw CScraperError(sTitle, sMessage);
}

std::unique_ptr<CScraper> CScraper::FromExtension(CAddonInfo addonInfo, const cp_extension_t* ext)
{
  bool requiressettings = CAddonMgr::GetInstance().GetExtValue(ext->configuration,"@requiressettings") == "true";

  CDateTimeSpan persistence;
  std::string tmp = CAddonMgr::GetInstance().GetExtValue(ext->configuration, "@cachepersistence");
  if (!tmp.empty())
    persistence.SetFromTimeString(tmp);

  CONTENT_TYPE pathContent(CONTENT_NONE);
  switch (addonInfo.MainType())
  {
    case ADDON_SCRAPER_ALBUMS:
      pathContent = CONTENT_ALBUMS;
      break;
    case ADDON_SCRAPER_ARTISTS:
      pathContent = CONTENT_ARTISTS;
      break;
    case ADDON_SCRAPER_MOVIES:
      pathContent = CONTENT_MOVIES;
      break;
    case ADDON_SCRAPER_MUSICVIDEOS:
      pathContent = CONTENT_MUSICVIDEOS;
      break;
    case ADDON_SCRAPER_TVSHOWS:
      pathContent = CONTENT_TVSHOWS;
      break;
    default:
      break;
  }

  return std::unique_ptr<CScraper>(new CScraper(std::move(addonInfo), requiressettings, persistence, pathContent));
}

CScraper::CScraper(CAddonInfo addonInfo)
  : CAddon(std::move(addonInfo)),
    m_fLoaded(false),
    m_requiressettings(false),
    m_pathContent(CONTENT_NONE)
{
  m_isPython = URIUtils::GetExtension(LibPath()) == ".py";
}

CScraper::CScraper(CAddonInfo addonInfo, bool requiressettings, CDateTimeSpan persistence, CONTENT_TYPE pathContent)
  : CAddon(std::move(addonInfo)),
    m_fLoaded(false),
    m_requiressettings(requiressettings),
    m_persistence(persistence),
    m_pathContent(pathContent)
{
  m_isPython = URIUtils::GetExtension(LibPath()) == ".py";
}

bool CScraper::Supports(const CONTENT_TYPE &content) const
{
  return Type() == ScraperTypeFromContent(content);
}

bool CScraper::SetPathSettings(CONTENT_TYPE content, const std::string& xml)
{
  m_pathContent = content;
  if (!LoadSettings(false, false))
    return false;

  if (xml.empty())
    return true;

  CXBMCTinyXML doc;
  doc.Parse(xml);
  return SettingsFromXML(doc);
}

std::string CScraper::GetPathSettings()
{
  if (!LoadSettings(false))
    return "";

  std::stringstream stream;
  CXBMCTinyXML doc;
  SettingsToXML(doc);
  if (doc.RootElement())
    stream << *doc.RootElement();

  return stream.str();
}

void CScraper::ClearCache()
{
  std::string strCachePath = URIUtils::AddFileToFolder(g_advancedSettings.m_cachePath, "scrapers");

  // create scraper cache dir if needed
  if (!CDirectory::Exists(strCachePath))
    CDirectory::Create(strCachePath);

  strCachePath = URIUtils::AddFileToFolder(strCachePath, ID());
  URIUtils::AddSlashAtEnd(strCachePath);

  if (CDirectory::Exists(strCachePath))
  {
    CFileItemList items;
    CDirectory::GetDirectory(strCachePath,items);
    for (int i=0;i<items.Size();++i)
    {
      // wipe cache
      if (items[i]->m_dateTime + m_persistence <= CDateTime::GetCurrentDateTime())
        CFile::Delete(items[i]->GetPath());
    }
  }
  else
    CDirectory::Create(strCachePath);
}

// returns a vector of strings: the first is the XML output by the function; the rest
// is XML output by chained functions, possibly recursively
// the CCurlFile object is passed in so that URL fetches can be canceled from other threads
// throws CScraperError abort on internal failures (e.g., parse errors)
std::vector<std::string> CScraper::Run(const std::string& function,
                                       const CScraperUrl& scrURL,
                                       CCurlFile& http,
                                       const std::vector<std::string>* extras)
{
  if (!Load())
    throw CScraperError();

  std::string strXML = InternalRun(function,scrURL,http,extras);
  if (strXML.empty())
  {
    if (function != "NfoUrl" && function != "ResolveIDToUrl")
      CLog::Log(LOGERROR, "%s: Unable to parse web site",__FUNCTION__);
    throw CScraperError();
  }

  CLog::Log(LOGDEBUG,"scraper: %s returned %s",function.c_str(),strXML.c_str());

  CXBMCTinyXML doc;
  /* all data was converted to UTF-8 before being processed by scraper */
  doc.Parse(strXML, TIXML_ENCODING_UTF8);
  if (!doc.RootElement())
  {
    CLog::Log(LOGERROR, "%s: Unable to parse XML",__FUNCTION__);
    throw CScraperError();
  }

  std::vector<std::string> result;
  result.push_back(strXML);
  TiXmlElement* xchain = doc.RootElement()->FirstChildElement();
  // skip children of the root element until <url> or <chain>
  while (xchain && strcmp(xchain->Value(),"url") && strcmp(xchain->Value(),"chain"))
      xchain = xchain->NextSiblingElement();
  while (xchain)
  {
    // <chain|url function="...">param</>
    const char* szFunction = xchain->Attribute("function");
    if (szFunction)
    {
      CScraperUrl scrURL2;
      std::vector<std::string> extras;
      // for <chain>, pass the contained text as a parameter; for <url>, as URL content
      if (strcmp(xchain->Value(),"chain")==0)
      {
        if (xchain->FirstChild())
          extras.push_back(xchain->FirstChild()->Value());
      }
      else
        scrURL2.ParseElement(xchain);
      // Fix for empty chains. $$1 would still contain the
      // previous value as there is no child of the xml node.
      // since $$1 will always either contain the data from an
      // url or the parameters to a chain, we can safely clear it here
      // to fix this issue
      m_parser.m_param[0].clear();
      std::vector<std::string> result2 = RunNoThrow(szFunction,scrURL2,http,&extras);
      result.insert(result.end(),result2.begin(),result2.end());
    }
    xchain = xchain->NextSiblingElement();
    // continue to skip past non-<url> or <chain> elements
    while (xchain && strcmp(xchain->Value(),"url") && strcmp(xchain->Value(),"chain"))
      xchain = xchain->NextSiblingElement();
  }

  return result;
}

// just like Run, but returns an empty list instead of throwing in case of error
// don't use in new code; errors should be handled appropriately
std::vector<std::string> CScraper::RunNoThrow(const std::string& function,
  const CScraperUrl& url,
  XFILE::CCurlFile& http,
  const std::vector<std::string>* extras)
{
  std::vector<std::string> vcs;
  try
  {
    vcs = Run(function, url, http, extras);
  }
  catch (const CScraperError &sce)
  {
    assert(sce.FAborted());  // the only kind we should get
  }
  return vcs;
}

std::string CScraper::InternalRun(const std::string& function,
                                 const CScraperUrl& scrURL,
                                 CCurlFile& http,
                                 const std::vector<std::string>* extras)
{
  // walk the list of input URLs and fetch each into parser parameters
  unsigned int i;
  for (i=0;i<scrURL.m_url.size();++i)
  {
    if (!CScraperUrl::Get(scrURL.m_url[i],m_parser.m_param[i],http,ID()) || m_parser.m_param[i].empty())
      return "";
  }
  // put the 'extra' parameterts into the parser parameter list too
  if (extras)
  {
    for (unsigned int j=0;j<extras->size();++j)
      m_parser.m_param[j+i] = (*extras)[j];
  }

  return m_parser.Parse(function,this);
}

bool CScraper::Load()
{
  if (m_fLoaded || m_isPython)
    return true;

  bool result=m_parser.Load(LibPath());
  if (result)
  {
    //! @todo this routine assumes that deps are a single level, and assumes the dep is installed.
    //!       1. Does it make sense to have recursive dependencies?
    //!       2. Should we be checking the dep versions or do we assume it is ok?
    ADDONDEPS deps = GetDeps();
    ADDONDEPS::iterator itr = deps.begin();
    while (itr != deps.end())
    {
      if (itr->first == "xbmc.metadata")
      {
        ++itr;
        continue;
      }
      AddonPtr dep;

      bool bOptional = itr->second.second;

      if (CAddonMgr::GetInstance().GetAddon((*itr).first, dep))
      {
        CXBMCTinyXML doc;
        if (dep->Type() == ADDON_SCRAPER_LIBRARY && doc.LoadFile(dep->LibPath()))
          m_parser.AddDocument(&doc);
      }
      else
      {
        if (!bOptional)
        {
          result = false;
          break;
        }
      }
      ++itr;
    }
  }

  if (!result)
    CLog::Log(LOGWARNING, "failed to load scraper XML from %s", LibPath().c_str());
  return m_fLoaded = result;
}

bool CScraper::IsInUse() const
{
  if (Supports(CONTENT_ALBUMS) || Supports(CONTENT_ARTISTS))
  { // music scraper
    CMusicDatabase db;
    if (db.Open() && db.ScraperInUse(ID()))
      return true;
  }
  else
  { // video scraper
    CVideoDatabase db;
    if (db.Open() && db.ScraperInUse(ID()))
      return true;
  }
  return false;
}

bool CScraper::IsNoop()
{
  if (!Load())
    throw CScraperError();

  return !m_isPython && m_parser.IsNoop();
}

// pass in contents of .nfo file; returns URL (possibly empty if none found)
// and may populate strId, or throws CScraperError on error
CScraperUrl CScraper::NfoUrl(const std::string &sNfoContent)
{
  CScraperUrl scurlRet;

  if (IsNoop())
    return scurlRet;

  // scraper function takes contents of .nfo file, returns XML (see below)
  std::vector<std::string> vcsIn;
  vcsIn.push_back(sNfoContent);
  CScraperUrl scurl;
  CCurlFile fcurl;
  std::vector<std::string> vcsOut = Run("NfoUrl", scurl, fcurl, &vcsIn);
  if (vcsOut.empty() || vcsOut[0].empty())
    return scurlRet;
  if (vcsOut.size() > 1)
    CLog::Log(LOGWARNING, "%s: scraper returned multiple results; using first", __FUNCTION__);

  // parse returned XML: either <error> element on error, blank on failure,
  // or <url>...</url> or <url>...</url><id>...</id> on success
  for (unsigned int i=0; i < vcsOut.size(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(vcsOut[i], TIXML_ENCODING_UTF8);
    CheckScraperError(doc.RootElement());

    if (doc.RootElement())
    {
      /*
       NOTE: Scrapers might return invalid xml with some loose
       elements (eg. '<url>http://some.url</url><id>123</id>').
       Since XMLUtils::GetString() is assuming well formed xml
       with start and end-tags we're not able to use it.
       Check for the desired Elements instead.
      */
      TiXmlElement* pxeUrl=NULL;
      TiXmlElement* pId=NULL;
      if (!strcmp(doc.RootElement()->Value(),"details"))
      {
        pxeUrl = doc.RootElement()->FirstChildElement("url");
        pId = doc.RootElement()->FirstChildElement("id");
      }
      else
      {
        pId = doc.FirstChildElement("id");
        pxeUrl = doc.FirstChildElement("url");
      }
      if (pId && pId->FirstChild())
        scurlRet.strId = pId->FirstChild()->Value();

      if (pxeUrl && pxeUrl->Attribute("function"))
        continue;

      if (pxeUrl)
        scurlRet.ParseElement(pxeUrl);
      else if (!strcmp(doc.RootElement()->Value(), "url"))
        scurlRet.ParseElement(doc.RootElement());
      else
        continue;
      break;
    }
  }
  return scurlRet;
}

CScraperUrl CScraper::ResolveIDToUrl(const std::string& externalID)
{
  CScraperUrl scurlRet;

  if (m_isPython)
  {
    std::stringstream str;
    str << "plugin://" << ID()
        << "?action=resolveid&key=" << CURL::Encode(externalID);

    CFileItem item("resolve me", false);

    if (XFILE::CPluginDirectory::GetPluginResult(str.str(), item))
      scurlRet.ParseString(item.GetPath());

    return scurlRet;
  }

  // scraper function takes an external ID, returns XML (see below)
  std::vector<std::string> vcsIn;
  vcsIn.push_back(externalID);
  CScraperUrl scurl;
  CCurlFile fcurl;
  std::vector<std::string> vcsOut = Run("ResolveIDToUrl", scurl, fcurl, &vcsIn);
  if (vcsOut.empty() || vcsOut[0].empty())
    return scurlRet;
  if (vcsOut.size() > 1)
    CLog::Log(LOGWARNING, "%s: scraper returned multiple results; using first", __FUNCTION__);

  // parse returned XML: either <error> element on error, blank on failure,
  // or <url>...</url> or <url>...</url><id>...</id> on success
  for (unsigned int i=0; i < vcsOut.size(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(vcsOut[i], TIXML_ENCODING_UTF8);
    CheckScraperError(doc.RootElement());

    if (doc.RootElement())
    {
      /*
       NOTE: Scrapers might return invalid xml with some loose
       elements (eg. '<url>http://some.url</url><id>123</id>').
       Since XMLUtils::GetString() is assuming well formed xml
       with start and end-tags we're not able to use it.
       Check for the desired Elements instead.
       */
      TiXmlElement* pxeUrl=NULL;
      TiXmlElement* pId=NULL;
      if (!strcmp(doc.RootElement()->Value(),"details"))
      {
        pxeUrl = doc.RootElement()->FirstChildElement("url");
        pId = doc.RootElement()->FirstChildElement("id");
      }
      else
      {
        pId = doc.FirstChildElement("id");
        pxeUrl = doc.FirstChildElement("url");
      }
      if (pId && pId->FirstChild())
        scurlRet.strId = pId->FirstChild()->Value();

      if (pxeUrl && pxeUrl->Attribute("function"))
        continue;

      if (pxeUrl)
        scurlRet.ParseElement(pxeUrl);
      else if (!strcmp(doc.RootElement()->Value(), "url"))
        scurlRet.ParseElement(doc.RootElement());
      else
        continue;
      break;
    }
  }
  return scurlRet;
}

static bool RelevanceSortFunction(const CScraperUrl &left, const CScraperUrl &right)
{
  return left.relevance > right.relevance;
}

template<class T>
static T FromFileItem(const CFileItem& item);


template<>
CScraperUrl FromFileItem<CScraperUrl>(const CFileItem& item)
{
  CScraperUrl url;

  url.strTitle = item.GetLabel();
  if (item.HasProperty("relevance"))
    url.relevance = item.GetProperty("relevance").asDouble();
  CScraperUrl::SUrlEntry surl;
  surl.m_type = CScraperUrl::URL_TYPE_GENERAL;
  surl.m_url =  item.GetPath();
  url.m_url.push_back(surl);

  return url;
}

template<>
CMusicAlbumInfo FromFileItem<CMusicAlbumInfo>(const CFileItem& item)
{
  CMusicAlbumInfo info;
  std::string sTitle  = item.GetLabel();
  std::string sArtist = item.GetProperty("album.artist").asString();
  std::string sAlbumName;
  if (!sArtist.empty())
    sAlbumName = StringUtils::Format("%s - %s", sArtist.c_str(), sTitle.c_str());
  else
    sAlbumName = sTitle;

  std::string sYear = item.GetProperty("album.year").asString();
  if (!sYear.empty())
    sAlbumName = StringUtils::Format("%s (%s)", sAlbumName.c_str(), sYear.c_str());

  CScraperUrl url;
  url.m_url.resize(1);
  url.m_url[0].m_url = item.GetPath();

  info = CMusicAlbumInfo(sTitle, sArtist, sAlbumName, url);
  if (item.HasProperty("relevance"))
    info.SetRelevance(item.GetProperty("relevance").asDouble());

  return info;
}

template<>
CMusicArtistInfo FromFileItem<CMusicArtistInfo>(const CFileItem& item)
{
  CMusicArtistInfo info;
  std::string sTitle  = item.GetLabel();

  CScraperUrl url;
  url.m_url.resize(1);
  url.m_url[0].m_url = item.GetPath();

  info = CMusicArtistInfo(sTitle, url);
  if (item.HasProperty("artist.genre"))
    info.GetArtist().genre = StringUtils::Split(item.GetProperty("artist.genre").asString(),
                                                g_advancedSettings.m_musicItemSeparator);
  if (item.HasProperty("artist.born"))
    info.GetArtist().strBorn = item.GetProperty("artist.born").asString();

  return info;
}


template<class T>
static std::vector<T>
PythonFind(const std::string& ID,
           const std::map<std::string, std::string>& additionals)
{
  std::vector<T> result;
  CFileItemList items;
  std::stringstream str;
  str << "plugin://" << ID << "?action=find";
  for (const auto& it : additionals)
    str << "&" << it.first<< "=" << CURL::Encode(it.second);

  if (XFILE::CDirectory::GetDirectory(str.str(), items))
  {
    for (int i = 0; i < items.Size(); ++i)
      result.emplace_back(std::move(FromFileItem<T>(*items[i])));
  }

  return result;
}

static std::string FromString(const CFileItem& item,
                              const std::string& key)
{
    return item.GetProperty(key).asString();
}

static std::vector<std::string> FromArray(const CFileItem& item,
                                          const std::string& key,
                                          int sep)
{
    return StringUtils::Split(item.GetProperty(key).asString(),
                              sep ? g_advancedSettings.m_videoItemSeparator :
                                    g_advancedSettings.m_musicItemSeparator);
}

static void ParseThumbs(CScraperUrl& scurl, const CFileItem& item,
                        int nThumbs, const std::string& tag)
{
  for (int i = 0; i < nThumbs; ++i)
  {
    std::stringstream prefix;
    prefix << tag << i+1;
    std::string url = FromString(item, prefix.str()+".url");
    std::string aspect = FromString(item, prefix.str()+".aspect");
    scurl.AddElement(url, aspect);
  }
}

static std::string ParseFanart(const CFileItem& item,
                               int nFanart, const std::string& tag)
{
  std::string result;
  TiXmlElement fanart("fanart");
  for (int i = 0; i < nFanart; ++i)
  {
    std::stringstream prefix;
    prefix << tag << i+1;
    std::string url = FromString(item, prefix.str()+".url");
    std::string preview = FromString(item, prefix.str()+".preview");
    TiXmlElement thumb("thumb");
    thumb.SetAttribute("preview", preview);
    TiXmlText text(url);
    thumb.InsertEndChild(text);
    fanart.InsertEndChild(thumb);
  }
  result << fanart;

  return result;
}

template<class T>
static void DetailsFromFileItem(const CFileItem&, T&);

template<>
void DetailsFromFileItem<CAlbum>(const CFileItem& item, CAlbum& album)
{
  album.strAlbum = item.GetLabel();
  album.strMusicBrainzAlbumID = FromString(item, "album.musicbrainzid");
  album.strReleaseGroupMBID = FromString(item, "album.releasegroupid");

  int nArtists = item.GetProperty("album.artists").asInteger();
  album.artistCredits.reserve(nArtists);
  for (int i = 0; i < nArtists; ++i)
  {
    std::stringstream prefix;
    prefix << "album.artist" << i+1;
    CArtistCredit artistCredit;
    artistCredit.SetArtist(FromString(item, prefix.str()+".name"));
    artistCredit.SetMusicBrainzArtistID(FromString(item, prefix.str()+".musicbrainzid"));
    album.artistCredits.push_back(artistCredit);
  }

  album.strArtistDesc = FromString(item, "album.artist_description");
  album.genre = FromArray(item, "album.genre", 0);
  album.styles = FromArray(item, "album.styles", 0);
  album.moods = FromArray(item, "album.moods", 0);
  album.themes = FromArray(item, "album.themes", 0);
  album.bCompilation = item.GetProperty("album.compilation").asBoolean();
  album.strReview = FromString(item, "album.review");
  album.m_strDateOfRelease = FromString(item, "album.release_date");
  album.strLabel = FromString(item, "album.label");
  album.strType = FromString(item, "album.type");
  album.SetReleaseType(FromString(item, "album.release_type"));
  album.iYear = item.GetProperty("album.year").asInteger();
  album.fRating = item.GetProperty("album.rating").asDouble();
  album.iUserrating = item.GetProperty("album.user_rating").asInteger();
  album.iVotes = item.GetProperty("album.votes").asInteger();

  int nThumbs = item.GetProperty("album.thumbs").asInteger();
  ParseThumbs(album.thumbURL, item, nThumbs, "album.thumb");
}

template<>
void DetailsFromFileItem<CArtist>(const CFileItem& item, CArtist& artist)
{
  artist.strArtist = item.GetLabel();
  artist.strMusicBrainzArtistID = FromString(item, "artist.musicbrainzid");
  artist.genre = FromArray(item, "artist.genre", 0);
  artist.styles = FromArray(item, "artist.styles", 0);
  artist.moods = FromArray(item, "artist.moods", 0);
  artist.yearsActive = FromArray(item, "artist.years_active", 0);
  artist.instruments = FromArray(item, "artist.instruments", 0);
  artist.strBorn = FromString(item, "artist.born");
  artist.strFormed = FromString(item, "artist.formed");
  artist.strBiography = FromString(item, "artist.biography");
  artist.strDied = FromString(item, "artist.died");
  artist.strDisbanded = FromString(item, "artist.disbanded");

  int nAlbums = item.GetProperty("artist.albums").asInteger();
  artist.discography.reserve(nAlbums);
  for (int i = 0; i < nAlbums; ++i)
  {
    std::stringstream prefix;
    prefix << "artist.album" << i+1;
    artist.discography.push_back(std::make_pair(FromString(item, prefix.str()+".title"),
                                                FromString(item, prefix.str()+".year")));
  }

  int nThumbs = item.GetProperty("artist.thumbs").asInteger();
  ParseThumbs(artist.thumbURL, item, nThumbs, "artist.thumb");

  int nFanart = item.GetProperty("artist.fanarts").asInteger();
  artist.fanart.m_xml = ParseFanart(item, nFanart, "artist.fanart");
  artist.fanart.Unpack();
}

template<>
void DetailsFromFileItem<CVideoInfoTag>(const CFileItem& item, CVideoInfoTag& tag)
{
  tag.SetTitle(item.GetLabel());
  tag.SetOriginalTitle(FromString(item, "video.original_title"));
  tag.SetShowTitle(FromString(item, "video.show_title"));
  tag.SetSortTitle(FromString(item, "video.sort_title"));
  int nRatings = item.GetProperty("video.ratings").asInteger();
  for (int i = 0; i < nRatings; ++i)
  {
    std::stringstream str;
    str << "video.rating" << i+1;
    int votes = item.GetProperty(str.str()+".votes").asInteger();
    float rating = item.GetProperty(str.str()+".value").asDouble();
    tag.SetRating(rating, votes, "default");
  }
  tag.m_iUserRating = item.GetProperty("video.user_rating").asInteger();
  tag.m_iTop250 = item.GetProperty("video.top250").asInteger();
  tag.m_iSeason = item.GetProperty("video.season").asInteger();
  tag.m_iEpisode = item.GetProperty("video.episode").asInteger();
  tag.m_iTrack = item.GetProperty("video.track").asInteger();
  tag.SetUniqueIDs({{"IMDB", FromString(item, "video.imdb_id")}});
  tag.m_iSpecialSortSeason = item.GetProperty("video.display_season").asInteger();
  tag.m_iSpecialSortEpisode = item.GetProperty("video.display_episode").asInteger();
  tag.SetPlotOutline(FromString(item, "video.plot_outline"));
  tag.SetPlot(FromString(item, "video.plot"));
  tag.SetTagLine(FromString(item, "video.tag_line"));
  tag.m_duration = item.GetProperty("video.duration_minutes").asInteger();
  tag.SetMPAARating(FromString(item, "video.mpaa"));
  tag.SetYear(item.GetProperty("video.premiere_year").asInteger());
  tag.SetStatus(FromString(item, "video.status"));
  tag.SetProductionCode(FromString(item, "video.production_code"));
  tag.m_firstAired.SetFromDBDate(FromString(item, "video.first_aired"));
  tag.SetAlbum(FromString(item, "video.album"));
  tag.SetTrailer(FromString(item, "video.trailer"));
  int nThumbs = item.GetProperty("video.thumbs").asInteger();
  ParseThumbs(tag.m_strPictureURL, item, nThumbs, "video.thumb");
  tag.SetGenre(FromArray(item, "video.genre", 1));
  tag.SetCountry(FromArray(item, "video.country", 1));
  tag.SetWritingCredits(FromArray(item, "video.writing_credits", 1));
  tag.SetDirector(FromArray(item, "video.director", 1));
  tag.SetShowLink(FromArray(item, "video.tvshow_links", 1));
  int nSeasons = item.GetProperty("video.seasons").asInteger();
  for (int i = 0; i < nSeasons; ++i)
  {
    std::stringstream str;
    str << "video.season" << i+1;
    tag.m_namedSeasons.insert(std::make_pair(i+1, FromString(item, str.str()+".name")));
  }
  int nActors = item.GetProperty("video.actors").asInteger();
  for (int i = 0; i< nActors; ++i)
  {
    std::stringstream str;
    str << "video.actor" << i+1;
    SActorInfo actor;
    actor.strName = FromString(item, str.str()+".name");
    actor.strRole = FromString(item, str.str()+".role");
    actor.order = item.GetProperty(str.str()+".sort_order").asInteger();
    std::string url = FromString(item, str.str()+".thumb");
    if (!url.empty())
    {
      double aspect = item.GetProperty(str.str()+".thumb_aspect").asDouble();
      TiXmlElement thumb("thumb");
      thumb.SetAttribute("aspect", aspect);
      TiXmlText text(url);
      thumb.InsertEndChild(text);
      actor.thumbUrl.ParseElement(&thumb);
    }
    tag.m_cast.push_back(actor);
  }

  tag.SetSet(FromString(item, "video.set_name"));
  tag.SetSetOverview(FromString(item, "video.set_overview"));
  tag.SetTags(FromArray(item, "video.tags", 1));
  tag.SetStudio(FromArray(item, "video.studio", 1));
  tag.SetArtist(FromArray(item, "video.artist", 1));
  tag.m_strEpisodeGuide = "<episodeguide>"+FromString(item, "video.episode_guide_url")+"</episodeguide>";
  int nFanart = item.GetProperty("video.fanarts").asInteger();
  tag.m_fanart.m_xml = ParseFanart(item, nFanart, "video.fanart");
  tag.m_fanart.Unpack();
  tag.m_firstAired.SetFromDBDate(FromString(item, "video.date_added"));
}

template<class T>
static bool PythonDetails(const std::string& ID,
                          const std::string& key,
                          const std::string& url,
                          const std::string& action,
                          T& result)
{
  std::stringstream str;
  str << "plugin://" << ID << "?action=" << action
      << "&" << key << "=" << CURL::Encode(url);

  CFileItem item(url, false);

  if (!XFILE::CPluginDirectory::GetPluginResult(str.str(), item))
    return false;

  DetailsFromFileItem(item, result);
  return true;
}


// fetch list of matching movies sorted by relevance (may be empty);
// throws CScraperError on error; first called with fFirst set, then unset if first try fails
std::vector<CScraperUrl> CScraper::FindMovie(XFILE::CCurlFile &fcurl,
                                             const std::string &sMovie,
                                             bool fFirst)
{
  // prepare parameters for URL creation
  std::string sTitle, sTitleYear, sYear;
  CUtil::CleanString(sMovie, sTitle, sTitleYear, sYear, true/*fRemoveExt*/, fFirst);

  CLog::Log(LOGDEBUG, "%s: Searching for '%s' using %s scraper "
    "(path: '%s', content: '%s', version: '%s')", __FUNCTION__, sTitle.c_str(),
    Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  std::vector<CScraperUrl> vcscurl;
  if (IsNoop())
    return vcscurl;

  if (!fFirst)
    StringUtils::Replace(sTitle, '-',' ');

  if (m_isPython)
  {
    std::map<std::string, std::string> additionals{{"title", sTitle}};
    if (!sYear.empty())
      additionals.insert({"year", sYear});
    return PythonFind<CScraperUrl>(ID(), additionals);
  }

  std::vector<std::string> vcsIn(1);
  g_charsetConverter.utf8To(SearchStringEncoding(), sTitle, vcsIn[0]);
  vcsIn[0] = CURL::Encode(vcsIn[0]);
  if (fFirst && !sYear.empty())
    vcsIn.push_back(sYear);

  // request a search URL from the title/filename/etc.
  CScraperUrl scurl;
  std::vector<std::string> vcsOut = Run("CreateSearchUrl", scurl, fcurl, &vcsIn);
  if (vcsOut.empty())
  {
    CLog::Log(LOGDEBUG, "%s: CreateSearchUrl failed", __FUNCTION__);
    throw CScraperError();
  }
  scurl.ParseString(vcsOut[0]);

  // do the search, and parse the result into a list
  vcsIn.clear();
  vcsIn.push_back(scurl.m_url[0].m_url);
  vcsOut = Run("GetSearchResults", scurl, fcurl, &vcsIn);

  bool fSort(true);
  std::set<std::string> stsDupeCheck;
  bool fResults(false);
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i, TIXML_ENCODING_UTF8);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML", __FUNCTION__);
      continue;  // might have more valid results later
    }

    CheckScraperError(doc.RootElement());

    TiXmlHandle xhDoc(&doc);
    TiXmlHandle xhResults = xhDoc.FirstChild("results");
    if (!xhResults.Element())
      continue;
    fResults = true;  // even if empty

    // we need to sort if returned results don't specify 'sorted="yes"'
    if (fSort)
    {
      const char *sorted = xhResults.Element()->Attribute("sorted");
      if (sorted != NULL)
        fSort = !StringUtils::EqualsNoCase(sorted, "yes");
    }

    for (TiXmlElement *pxeMovie = xhResults.FirstChild("entity").Element();
      pxeMovie; pxeMovie = pxeMovie->NextSiblingElement())
    {
      CScraperUrl scurlMovie;
      TiXmlNode *pxnTitle = pxeMovie->FirstChild("title");
      TiXmlElement *pxeLink = pxeMovie->FirstChildElement("url");
      if (pxnTitle && pxnTitle->FirstChild() && pxeLink && pxeLink->FirstChild())
      {
        scurlMovie.strTitle = pxnTitle->FirstChild()->Value();
        XMLUtils::GetString(pxeMovie, "id", scurlMovie.strId);

        for ( ; pxeLink && pxeLink->FirstChild(); pxeLink = pxeLink->NextSiblingElement("url"))
          scurlMovie.ParseElement(pxeLink);

        // calculate the relevance of this hit
        std::string sCompareTitle = scurlMovie.strTitle;
        StringUtils::ToLower(sCompareTitle);
        std::string sMatchTitle = sTitle;
        StringUtils::ToLower(sMatchTitle);

        /*
         * Identify the best match by performing a fuzzy string compare on the search term and
         * the result. Additionally, use the year (if available) to further refine the best match.
         * An exact match scores 1, a match off by a year scores 0.5 (release dates can vary between
         * countries), otherwise it scores 0.
         */
        std::string sCompareYear;
        XMLUtils::GetString(pxeMovie, "year", sCompareYear);

        double yearScore = 0;
        if (!sYear.empty() && !sCompareYear.empty())
          yearScore = std::max(0.0, 1-0.5*abs(atoi(sYear.c_str())-atoi(sCompareYear.c_str())));

        scurlMovie.relevance = fstrcmp(sMatchTitle.c_str(), sCompareTitle.c_str(), 0.0) + yearScore;

        // reconstruct a title for the user
        if (!sCompareYear.empty())
          scurlMovie.strTitle += StringUtils::Format(" (%s)", sCompareYear.c_str());

        std::string sLanguage;
        if (XMLUtils::GetString(pxeMovie, "language", sLanguage) && !sLanguage.empty())
          scurlMovie.strTitle += StringUtils::Format(" (%s)", sLanguage.c_str());

        // filter for dupes from naughty scrapers
        if (stsDupeCheck.insert(scurlMovie.m_url[0].m_url + " " + scurlMovie.strTitle).second)
          vcscurl.push_back(scurlMovie);
      }
    }
  }

  if (!fResults)
    throw CScraperError();  // scraper aborted

  if (fSort)
    std::stable_sort(vcscurl.begin(), vcscurl.end(), RelevanceSortFunction);

  return vcscurl;
}

// find album by artist, using fcurl for web fetches
// returns a list of albums (empty if no match or failure)
std::vector<CMusicAlbumInfo> CScraper::FindAlbum(CCurlFile &fcurl,
                                                 const std::string &sAlbum,
                                                 const std::string &sArtist)
{
  CLog::Log(LOGDEBUG, "%s: Searching for '%s - %s' using %s scraper "
    "(path: '%s', content: '%s', version: '%s')", __FUNCTION__, sArtist.c_str(),
    sAlbum.c_str(), Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  std::vector<CMusicAlbumInfo> vcali;
  if (IsNoop())
    return vcali;

  if (m_isPython)
    return PythonFind<CMusicAlbumInfo>(ID(), {{"title", sAlbum},
                                              {"artist", sArtist}});

  // scraper function is given the album and artist as parameters and
  // returns an XML <url> element parseable by CScraperUrl
  std::vector<std::string> extras(2);
  g_charsetConverter.utf8To(SearchStringEncoding(), sAlbum, extras[0]);
  g_charsetConverter.utf8To(SearchStringEncoding(), sArtist, extras[1]);
  extras[0] = CURL::Encode(extras[0]);
  extras[1] = CURL::Encode(extras[1]);
  CScraperUrl scurl;
  std::vector<std::string> vcsOut = RunNoThrow("CreateAlbumSearchUrl", scurl, fcurl, &extras);
  if (vcsOut.size() > 1)
    CLog::Log(LOGWARNING, "%s: scraper returned multiple results; using first", __FUNCTION__);

  if (vcsOut.empty() || vcsOut[0].empty())
    return vcali;
  scurl.ParseString(vcsOut[0]);

  // the next function is passed the contents of the returned URL, and returns
  // an empty string on failure; on success, returns XML matches in the form:
  // <results>
  //  <entity>
  //   <title>...</title>
  //   <url>...</url> (with the usual CScraperUrl decorations like post or spoof)
  //   <artist>...</artist>
  //   <year>...</year>
  //   <relevance [scale="..."]>...</relevance> (scale defaults to 1; score is divided by it)
  //  </entity>
  //  ...
  // </results>
  vcsOut = RunNoThrow("GetAlbumSearchResults", scurl, fcurl);

  // parse the returned XML into a vector of album objects
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i, TIXML_ENCODING_UTF8);
    TiXmlHandle xhDoc(&doc);

    for (TiXmlElement* pxeAlbum = xhDoc.FirstChild("results").FirstChild("entity").Element();
      pxeAlbum; pxeAlbum = pxeAlbum->NextSiblingElement())
    {
      std::string sTitle;
      if (XMLUtils::GetString(pxeAlbum, "title", sTitle) && !sTitle.empty())
      {
        std::string sArtist;
        std::string sAlbumName;
        if (XMLUtils::GetString(pxeAlbum, "artist", sArtist) && !sArtist.empty())
          sAlbumName = StringUtils::Format("%s - %s", sArtist.c_str(), sTitle.c_str());
        else
          sAlbumName = sTitle;

        std::string sYear;
        if (XMLUtils::GetString(pxeAlbum, "year", sYear) && !sYear.empty())
          sAlbumName = StringUtils::Format("%s (%s)", sAlbumName.c_str(), sYear.c_str());

        // if no URL is provided, use the URL we got back from CreateAlbumSearchUrl
        // (e.g., in case we only got one result back and were sent to the detail page)
        TiXmlElement* pxeLink = pxeAlbum->FirstChildElement("url");
        CScraperUrl scurlAlbum;
        if (!pxeLink)
          scurlAlbum.ParseString(scurl.m_xml);
        for ( ; pxeLink && pxeLink->FirstChild(); pxeLink = pxeLink->NextSiblingElement("url"))
          scurlAlbum.ParseElement(pxeLink);

        if (!scurlAlbum.m_url.size())
          continue;

        CMusicAlbumInfo ali(sTitle, sArtist, sAlbumName, scurlAlbum);

        TiXmlElement* pxeRel = pxeAlbum->FirstChildElement("relevance");
        if (pxeRel && pxeRel->FirstChild())
        {
          const char* szScale = pxeRel->Attribute("scale");
          float flScale = szScale ? float(atof(szScale)) : 1;
          ali.SetRelevance(float(atof(pxeRel->FirstChild()->Value())) / flScale);
        }

        vcali.push_back(ali);
      }
    }
  }
  return vcali;
}

// find artist, using fcurl for web fetches
// returns a list of artists (empty if no match or failure)
std::vector<CMusicArtistInfo> CScraper::FindArtist(CCurlFile &fcurl,
                                                   const std::string &sArtist)
{
  CLog::Log(LOGDEBUG, "%s: Searching for '%s' using %s scraper "
    "(file: '%s', content: '%s', version: '%s')", __FUNCTION__, sArtist.c_str(),
    Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  std::vector<CMusicArtistInfo> vcari;
  if (IsNoop())
    return vcari;

  if (m_isPython)
    return PythonFind<CMusicArtistInfo>(ID(), {{"artist", sArtist}});

  // scraper function is given the artist as parameter and
  // returns an XML <url> element parseable by CScraperUrl
  std::vector<std::string> extras(1);
  g_charsetConverter.utf8To(SearchStringEncoding(), sArtist, extras[0]);
  extras[0] = CURL::Encode(extras[0]);
  CScraperUrl scurl;
  std::vector<std::string> vcsOut = RunNoThrow("CreateArtistSearchUrl", scurl, fcurl, &extras);

  if (vcsOut.empty() || vcsOut[0].empty())
    return vcari;
  scurl.ParseString(vcsOut[0]);

  // the next function is passed the contents of the returned URL, and returns
  // an empty string on failure; on success, returns XML matches in the form:
  // <results>
  //  <entity>
  //   <title>...</title>
  //   <year>...</year>
  //   <genre>...</genre>
  //   <url>...</url> (with the usual CScraperUrl decorations like post or spoof)
  //  </entity>
  //  ...
  // </results>
  vcsOut = RunNoThrow("GetArtistSearchResults", scurl, fcurl);

  // parse the returned XML into a vector of artist objects
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i, TIXML_ENCODING_UTF8);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML", __FUNCTION__);
      return vcari;
    }
    TiXmlHandle xhDoc(&doc);
    for (TiXmlElement* pxeArtist = xhDoc.FirstChild("results").FirstChild("entity").Element();
      pxeArtist; pxeArtist = pxeArtist->NextSiblingElement())
    {
      TiXmlNode* pxnTitle = pxeArtist->FirstChild("title");
      if (pxnTitle && pxnTitle->FirstChild())
      {
        CScraperUrl scurlArtist;

        TiXmlElement* pxeLink = pxeArtist->FirstChildElement("url");
        if (!pxeLink)
          scurlArtist.ParseString(scurl.m_xml);
        for ( ; pxeLink && pxeLink->FirstChild(); pxeLink = pxeLink->NextSiblingElement("url"))
          scurlArtist.ParseElement(pxeLink);

        if (!scurlArtist.m_url.size())
          continue;

        CMusicArtistInfo ari(pxnTitle->FirstChild()->Value(), scurlArtist);
        std::string genre;
        XMLUtils::GetString(pxeArtist, "genre", genre);
        if (!genre.empty())
          ari.GetArtist().genre = StringUtils::Split(genre, g_advancedSettings.m_musicItemSeparator);
        XMLUtils::GetString(pxeArtist, "year", ari.GetArtist().strBorn);

        vcari.push_back(ari);
      }
    }
  }
  return vcari;
}

// fetch list of episodes from URL (from video database)
EPISODELIST CScraper::GetEpisodeList(XFILE::CCurlFile &fcurl, const CScraperUrl &scurl)
{
  EPISODELIST vcep;
  if (scurl.m_url.empty())
    return vcep;

  CLog::Log(LOGDEBUG, "%s: Searching '%s' using %s scraper "
    "(file: '%s', content: '%s', version: '%s')", __FUNCTION__,
    scurl.m_url[0].m_url.c_str(), Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  if (m_isPython)
  {
    std::stringstream str;
    str << "plugin://" << ID() << "?action=getepisodelist&url="
        << CURL::Encode(scurl.m_url.front().m_url);
    CFileItemList items;
    if (!XFILE::CDirectory::GetDirectory(str.str(), items))
      return vcep;

    for (int i = 0; i < items.Size(); ++i)
    {
      EPISODE ep;
      ep.strTitle = items[i]->GetLabel();
      ep.iSeason = items[i]->GetProperty("video.season").asInteger();
      ep.iEpisode = items[i]->GetProperty("video.episode").asInteger();
      ep.iSubepisode = items[i]->GetProperty("video.sub_episode").asInteger();
      CScraperUrl::SUrlEntry surl;
      surl.m_type = CScraperUrl::URL_TYPE_GENERAL;
      surl.m_url =  FromString(*items[i], "video.url");
      ep.cScraperUrl.m_url.push_back(surl);
      ep.cScraperUrl.strId = FromString(*items[i], "video.unique_id");
      vcep.push_back(ep);
    }

    return vcep;
  }

  std::vector<std::string> vcsIn;
  vcsIn.push_back(scurl.m_url[0].m_url);
  std::vector<std::string> vcsOut = RunNoThrow("GetEpisodeList", scurl, fcurl, &vcsIn);

  // parse the XML response
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML",__FUNCTION__);
      continue;
    }

    TiXmlHandle xhDoc(&doc);
    for (TiXmlElement *pxeMovie = xhDoc.FirstChild("episodeguide").FirstChild("episode").
      Element(); pxeMovie; pxeMovie = pxeMovie->NextSiblingElement())
    {
      EPISODE ep;
      TiXmlElement *pxeLink = pxeMovie->FirstChildElement("url");
      std::string strEpNum;
      if (pxeLink && XMLUtils::GetInt(pxeMovie, "season", ep.iSeason) &&
        XMLUtils::GetString(pxeMovie, "epnum", strEpNum) && !strEpNum.empty())
      {
        CScraperUrl &scurlEp(ep.cScraperUrl);
        size_t dot = strEpNum.find(".");
        ep.iEpisode = atoi(strEpNum.c_str());
        ep.iSubepisode = (dot != std::string::npos) ? atoi(strEpNum.substr(dot + 1).c_str()) : 0;
        if (!XMLUtils::GetString(pxeMovie, "title", scurlEp.strTitle) || scurlEp.strTitle.empty() )
            scurlEp.strTitle = g_localizeStrings.Get(416);
        XMLUtils::GetString(pxeMovie, "id", scurlEp.strId);

        for ( ; pxeLink && pxeLink->FirstChild(); pxeLink = pxeLink->NextSiblingElement("url"))
          scurlEp.ParseElement(pxeLink);

        // date must be the format of yyyy-mm-dd
        ep.cDate.SetValid(FALSE);
        std::string sDate;
        if (XMLUtils::GetString(pxeMovie, "aired", sDate) && sDate.length() == 10)
        {
          tm tm;
          if (strptime(sDate.c_str(), "%Y-%m-%d", &tm))
            ep.cDate.SetDate(1900+tm.tm_year, tm.tm_mon + 1, tm.tm_mday);
        }
        vcep.push_back(ep);
      }
    }
  }

  return vcep;
}

// takes URL; returns true and populates video details on success, false otherwise
bool CScraper::GetVideoDetails(XFILE::CCurlFile &fcurl,
                               const CScraperUrl &scurl,
                               bool fMovie/*else episode*/,
                               CVideoInfoTag &video)
{
  CLog::Log(LOGDEBUG, "%s: Reading %s '%s' using %s scraper "
    "(file: '%s', content: '%s', version: '%s')", __FUNCTION__,
    fMovie ? MediaTypeMovie : MediaTypeEpisode, scurl.m_url[0].m_url.c_str(), Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  video.Reset();

  if (m_isPython)
    return PythonDetails(ID(), "url", scurl.m_url.front().m_url,
                         fMovie ? "getdetails" : "getepisodedetails", video);

  std::string sFunc = fMovie ? "GetDetails" : "GetEpisodeDetails";
  std::vector<std::string> vcsIn;
  vcsIn.push_back(scurl.strId);
  vcsIn.push_back(scurl.m_url[0].m_url);
  std::vector<std::string> vcsOut = RunNoThrow(sFunc, scurl, fcurl, &vcsIn);

  // parse XML output
  bool fRet(false);
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i, TIXML_ENCODING_UTF8);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML", __FUNCTION__);
      continue;
    }

    TiXmlHandle xhDoc(&doc);
    TiXmlElement *pxeDetails = xhDoc.FirstChild("details").Element();
    if (!pxeDetails)
    {
      CLog::Log(LOGERROR, "%s: Invalid XML file (want <details>)", __FUNCTION__);
      continue;
    }
    video.Load(pxeDetails, true/*fChain*/);
    fRet = true;  // but don't exit in case of chaining
  }
  return fRet;
}

// takes a URL; returns true and populates album on success, false otherwise
bool CScraper::GetAlbumDetails(CCurlFile &fcurl, const CScraperUrl &scurl, CAlbum &album)
{
  CLog::Log(LOGDEBUG, "%s: Reading '%s' using %s scraper "
    "(file: '%s', content: '%s', version: '%s')", __FUNCTION__,
    scurl.m_url[0].m_url.c_str(), Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  if (m_isPython)
    return PythonDetails(ID(), "url", scurl.m_url.front().m_url,
                         "getdetails", album);

  std::vector<std::string> vcsOut = RunNoThrow("GetAlbumDetails", scurl, fcurl);

  // parse the returned XML into an album object (see CAlbum::Load for details)
  bool fRet(false);
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i, TIXML_ENCODING_UTF8);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML", __FUNCTION__);
      return false;
    }
    fRet = album.Load(doc.RootElement(), i != vcsOut.begin());
  }
  return fRet;
}

// takes a URL (one returned from FindArtist), the original search string, and
// returns true and populates artist on success, false on failure
bool CScraper::GetArtistDetails(CCurlFile &fcurl, const CScraperUrl &scurl,
  const std::string &sSearch, CArtist &artist)
{
  if (!scurl.m_url.size())
    return false;

  CLog::Log(LOGDEBUG, "%s: Reading '%s' ('%s') using %s scraper "
    "(file: '%s', content: '%s', version: '%s')", __FUNCTION__,
    scurl.m_url[0].m_url.c_str(), sSearch.c_str(), Name().c_str(), Path().c_str(),
    ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  if (m_isPython)
    return PythonDetails(ID(), "url", scurl.m_url.front().m_url,
                         "getdetails", artist);

  // pass in the original search string for chaining to search other sites
  std::vector<std::string> vcIn;
  vcIn.push_back(sSearch);
  vcIn[0] = CURL::Encode(vcIn[0]);

  std::vector<std::string> vcsOut = RunNoThrow("GetArtistDetails", scurl, fcurl, &vcIn);

  // ok, now parse the xml file
  bool fRet(false);
  for (std::vector<std::string>::const_iterator i = vcsOut.begin(); i != vcsOut.end(); ++i)
  {
    CXBMCTinyXML doc;
    doc.Parse(*i, TIXML_ENCODING_UTF8);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML", __FUNCTION__);
      return false;
    }

    fRet = artist.Load(doc.RootElement(), i != vcsOut.begin());
  }
  return fRet;
}

bool CScraper::GetArtwork(XFILE::CCurlFile &fcurl, CVideoInfoTag &details)
{
  if (!details.HasUniqueID())
    return false;

  CLog::Log(LOGDEBUG, "%s: Reading artwork for '%s' using %s scraper "
    "(file: '%s', content: '%s', version: '%s')", __FUNCTION__, details.GetUniqueID().c_str(),
    Name().c_str(), Path().c_str(), ADDON::TranslateContent(Content()).c_str(), Version().asString().c_str());

  if (m_isPython)
    return PythonDetails(ID(), "id", details.GetUniqueID("IMDB"), "getartwork", details);

  std::vector<std::string> vcsIn;
  CScraperUrl scurl;
  vcsIn.push_back(details.GetUniqueID());
  std::vector<std::string> vcsOut = RunNoThrow("GetArt", scurl, fcurl, &vcsIn);

  bool fRet(false);
  for (std::vector<std::string>::const_iterator it = vcsOut.begin(); it != vcsOut.end(); ++it)
  {
    CXBMCTinyXML doc;
    doc.Parse(*it, TIXML_ENCODING_UTF8);
    if (!doc.RootElement())
    {
      CLog::Log(LOGERROR, "%s: Unable to parse XML", __FUNCTION__);
      return false;
    }
    fRet = details.Load(doc.RootElement(), it != vcsOut.begin());
  }
  return fRet;
}

}
