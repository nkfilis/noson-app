/*
 *      Copyright (C) 2016 Jean-Luc Barriere
 *
 *  This library is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation; either version 3, or (at your option)
 *  any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301 USA
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "smapi.h"
#include "private/debug.h"
#include "private/os/threads/mutex.h"
#include "private/tinyxml2.h"
#include "private/xmldict.h"
#include "private/wsresponse.h"
#include "private/builtin.h"
#include "private/cppdef.h"

#define DEVICE_PROVIDER         "Sonos"
#define SMAPI_NAMESPACE         "http://www.sonos.com/Services/1.1"
#define SOAP_ENVELOPE_NAMESPACE "http://schemas.xmlsoap.org/soap/envelope/"
#define SOAP_ENCODING_NAMESPACE "http://schemas.xmlsoap.org/soap/encoding/"

using namespace NSROOT;

namespace NSROOT
{
  XMLDict __initSMAPIDict()
  {
    XMLDict dict;
    dict.DefineNS("s", SOAP_ENVELOPE_NAMESPACE);
    dict.DefineNS("", SMAPI_NAMESPACE);
    return dict;
  }
  XMLDict SMAPIDict = __initSMAPIDict();

  void __dumpInvalidResponse(tinyxml2::XMLDocument& doc)
  {
    DBG(DBG_ERROR, "%s: invalid or not supported response\n", __FUNCTION__);
    tinyxml2::XMLPrinter out;
    doc.Accept(&out);
    DBG(DBG_ERROR, "%s\n", out.CStr());
  }
}

SMAPI::SMAPI(const PlayerPtr& player)
: m_mutex(new OS::CMutex)
, m_player(player)
, m_deviceSerialNumber()
, m_deviceHouseholdID()
, m_service(0)
, m_uri(0)
, m_valid(false)
{
  ElementList vars;
  ElementList::const_iterator it;
  if (player->GetZoneInfo(vars) && (it = vars.FindKey("SerialNumber")) != vars.end())
    m_deviceSerialNumber = **it;
  if (player->GetHouseholdID(vars) && (it = vars.FindKey("CurrentHouseholdID")) != vars.end())
    m_deviceHouseholdID = **it;
}

SMAPI::~SMAPI()
{
  SAFE_DELETE(m_uri);
  SAFE_DELETE(m_mutex);
}

bool SMAPI::Init(const SMServicePtr& smsvc)
{
  OS::CLockGuard lock(*m_mutex);
  m_valid = false;
  // store selected service
  m_service = smsvc;
  if (!m_service)
    return false;
  if (!m_service->GetPresentationMap())
  {
    m_presentation.clear();
    m_searchCategories.clear();
    // create a default search for service 'TuneIn'
    if (m_service->GetName() == "TuneIn")
    {
      m_searchCategories.push_back(ElementPtr(new Element("stations", "search:station")));
    }
  }
  else
  {
    // load presentation map from given uri
    URIParser uri(m_service->GetPresentationMap()->GetAttribut("Uri"));
    WSRequest request(uri);
    request.SetUserAgent(m_service->GetAgent());
    WSResponse response(request);
    if (!response.IsSuccessful())
    {
      DBG(DBG_ERROR, "%s: invalid response\n", __FUNCTION__);
      return false;
    }
    // receive content data
    size_t len = 0, l = 0;
    std::string data;
    char buffer[4096];
    while ((l = response.ReadContent(buffer, sizeof(buffer))))
    {
      data.append(buffer, l);
      len += l;
    }
    if (!parsePresentationMap(data))
      return false;
  }

  // setup end-point from service URI
  if (m_service->GetSecureUri().empty())
    m_uri = new URIParser(m_service->GetUri());
  else
    m_uri = new URIParser(m_service->GetSecureUri());

  // make the soap header
  if (!makeSoapHeader())
    return false;

  return m_valid = true;
}

bool SMAPI::GetMetadata(const std::string& id, int index, int count, bool recursive, SMAPIMetadata& metadata)
{
  char buf[20];
  ElementList args;
  args.push_back(ElementPtr(new Element("id", id)));
  int32_to_string(index, buf);
  args.push_back(ElementPtr(new Element("index", buf)));
  int32_to_string(count, buf);
  args.push_back(ElementPtr(new Element("count", buf)));
  args.push_back(ElementPtr(new Element("recursive", recursive ? "true" : "false")));

  metadata.Reset(m_service, Request("getMetadata", args).GetValue("getMetadataResult"), id);
  return metadata.IsValid();
}

bool SMAPI::GetMediaMetadata(const std::string& id, SMAPIMetadata& metadata)
{
  ElementList args;
  args.push_back(ElementPtr(new Element("id", id)));

  metadata.Reset(m_service, Request("getMediaMetadata", args).GetValue("getMediaMetadataResult"), id);
  return metadata.IsValid();
}

bool SMAPI::Search(const std::string& searchId, const std::string& term, int index, int count, SMAPIMetadata& metadata)
{
  const std::string& mappedId = m_searchCategories.GetValue(searchId);
  if (mappedId.empty())
    return false;
  char buf[20];
  ElementList args;
  args.push_back(ElementPtr(new Element("id", mappedId)));
  args.push_back(ElementPtr(new Element("term", term)));
  int32_to_string(index, buf);
  args.push_back(ElementPtr(new Element("index", buf)));
  int32_to_string(count, buf);
  args.push_back(ElementPtr(new Element("count", buf)));

  metadata.Reset(m_service, Request("search", args).GetValue("searchResult"), mappedId);
  return metadata.IsValid();
}

bool SMAPI::parsePresentationMap(const std::string& xml)
{
  tinyxml2::XMLDocument rootdoc;
  // Parse xml content
  if (rootdoc.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS)
  {
    DBG(DBG_ERROR, "%s: parse xml failed\n", __FUNCTION__);
    return false;
  }
  tinyxml2::XMLElement* elem; // an element
  // Check for response: Presentation
  if (!(elem = rootdoc.RootElement()) || !XMLNS::NameEqual(elem->Name(), "Presentation"))
  {
    DBG(DBG_ERROR, "%s: invalid or not supported content\n", __FUNCTION__);
    tinyxml2::XMLPrinter out;
    rootdoc.Accept(&out);
    DBG(DBG_ERROR, "%s\n", out.CStr());
    return false;
  }
  m_presentation.clear();
  m_searchCategories.clear();
  elem = elem->FirstChildElement("PresentationMap");
  while (elem)
  {
    unsigned uid = 0; // unique item id
    char sid[10];
    memset(sid, '\0', sizeof(sid));
    tinyxml2::XMLElement* child; // a child of elem
    const char* type = elem->Attribute("type");
    if (type)
    {
      if (strncmp(type, "DisplayType", 11) == 0)
      {
      }
      else if (strncmp(type, "Search", 6) == 0 && (child = elem->FirstChildElement("Match")))
      {
        child = child->FirstChildElement("SearchCategories");
        while (child)
        {
          ElementPtr search(new Element("Search"));
          // set attribute StringId if any
          const char* stringId = child->Attribute("stringId");
          if (stringId)
            search->SetAttribut("stringId", stringId);
          // build the list of category for this search categories
          ElementList list;
          tinyxml2::XMLElement* categ = child->FirstChildElement();
          while (categ && categ->Attribute("id") && categ->Attribute("mappedId"))
          {
            // could be Category or CustomCategory
            uint32_to_string(++uid, sid);
            ElementPtr item(new Element(categ->Name(), sid));
            item->SetAttribut("id", categ->Attribute("id"));
            item->SetAttribut("mappedId", categ->Attribute("mappedId"));
            list.push_back(item);
            // also fill list of search categories
            m_searchCategories.push_back(ElementPtr(new Element(categ->Attribute("id"), categ->Attribute("mappedId"))));
            categ = categ->NextSiblingElement(NULL);
          }
          m_presentation.push_back(std::make_pair(search, list));
          child = child->NextSiblingElement(NULL);
        }
      }
      else if (strncmp(type, "BrowseIconSizeMap", 17) == 0 && (child = elem->FirstChildElement("Match")))
      {
        child = child->FirstChildElement("browseIconSizeMap");
        if (child)
        {
          ElementPtr name(new Element(child->Name()));
          // build the list of size entry
          ElementList list;
          tinyxml2::XMLElement* entry = child->FirstChildElement("sizeEntry");
          while (entry && entry->Attribute("size") && entry->Attribute("substitution"))
          {
            uint32_to_string(++uid, sid);
            ElementPtr item(new Element(entry->Name(), sid));
            item->SetAttribut("size", entry->Attribute("size"));
            item->SetAttribut("substitution", entry->Attribute("substitution"));
            list.push_back(item);
            entry = entry->NextSiblingElement(NULL);
          }
          m_presentation.push_back(std::make_pair(name, list));
        }
      }
      else if (strncmp(type, "NowPlayingRatings", 17) == 0)
      {
      }
    }
    elem = elem->NextSiblingElement(NULL);
  }
  // Storage for presentation:
  // Element: key=Search, attr={stringId}
  // values : key=Category, attr={id="stations", mappedId="search:station"}, value=#ordered#
  //
  // Element: key=browseIconSizeMap, attr={}
  // values : key=sizeEntry, attr={size="0", substitution="_legacy.svg"}, value=#ordered#
  //
  return true;
}

bool SMAPI::makeSoapHeader()
{
  m_soapHeader.append("<credentials xmlns=\"" SMAPI_NAMESPACE "\">");

  SMAccount::OACredentials auth = m_service->GetAccount()->GetOACredentials();
  if (!auth.token.empty())
  {
    m_soapHeader.append("<deviceId>").append(m_deviceSerialNumber).append("</deviceId>");
    m_soapHeader.append("<deviceProvider>" DEVICE_PROVIDER "</deviceProvider>");
    m_soapHeader.append("<loginToken>");
    m_soapHeader.append("<token>").append(auth.token).append("</token>");
    m_soapHeader.append("<key>").append(auth.key).append("</key>");
    m_soapHeader.append("<householdId>").append(auth.devId).append("</householdId>");
    m_soapHeader.append("</loginToken>");
  }
  else
  {
    m_soapHeader.append("<deviceId>").append(m_deviceSerialNumber).append("</deviceId>");
    m_soapHeader.append("<deviceProvider>" DEVICE_PROVIDER "</deviceProvider>");
    std::string policyAuth = m_service->GetPolicy()->GetAttribut("Auth");
    if (policyAuth == "UserId" || policyAuth == "DeviceLink")
    {
      ElementList vars;
      m_player->GetSessionId(m_service->GetId(), m_service->GetAccount()->GetUserName(), vars);
      m_soapHeader.append("<sessionId>").append(vars.GetValue("SessionId")).append("</sessionId>");
    }
  }

  m_soapHeader.append("</credentials>");
  return true;
}

ElementList SMAPI::DoCall(const std::string& action, const ElementList& args)
{
  ElementList vars;

  std::string soapaction;
  soapaction.append("\"" SMAPI_NAMESPACE "#").append(action).append("\"");

  std::string content;
  content.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
  // start envelope
  content.append("<s:Envelope xmlns:s=\"" SOAP_ENVELOPE_NAMESPACE "\" s:encodingStyle=\"" SOAP_ENCODING_NAMESPACE "\">");
  // fill the header
  content.append("<s:Header>").append(m_soapHeader).append("</s:Header>");
  // start body
  content.append("<s:Body>");
  content.append("<ns:").append(action).append(" xmlns:ns=\"" SMAPI_NAMESPACE "\">");
  for (ElementList::const_iterator it = args.begin(); it != args.end(); ++it)
    content.append((*it)->XML("ns"));
  content.append("</ns:").append(action).append(">");
  // end body
  content.append("</s:Body>");
  // end envelope
  content.append("</s:Envelope>");

  WSRequest request(*m_uri, HRM_POST);
  request.SetUserAgent(m_service->GetAgent());
  request.SetHeader("SOAPAction", soapaction);
  request.SetContentCustom(CT_XML, content.c_str());
  WSResponse response(request);

  // don't check response status code
  // service will return 500 on soap fault

  // Receive content data
  size_t len = 0, l = 0;
  std::string data;
  char buffer[4096];
  while ((l = response.ReadContent(buffer, sizeof(buffer))))
  {
    data.append(buffer, l);
    len += l;
  }

  // Parse xml content
  tinyxml2::XMLDocument rootdoc;
  if (rootdoc.Parse(data.c_str(), len) != tinyxml2::XML_SUCCESS)
  {
    DBG(DBG_ERROR, "%s: parse xml failed\n", __FUNCTION__);
    return vars;
  }
  const tinyxml2::XMLElement* elem; // an element
  // Check for response: Envelope/Body/{respTag}

  if (!(elem = rootdoc.RootElement()) || !XMLNS::NameEqual(elem->Name(), "Envelope"))
  {
    __dumpInvalidResponse(rootdoc);
    return vars;
  }
  // learn declared namespaces in the element Envelope for translations
  XMLNames xmlnames;
  xmlnames.AddXMLNS(elem);

  if (!(elem = elem->FirstChildElement()) || !XMLNS::NameEqual(elem->Name(), "Body") ||
          !(elem = elem->FirstChildElement()))
  {
    __dumpInvalidResponse(rootdoc);
    return vars;
  }
  vars.push_back(ElementPtr(new Element("TAG", XMLNS::LocalName(elem->Name()))));
  if (vars.back()->compare("Fault") == 0)
  {
    const tinyxml2::XMLElement* felem = elem->FirstChildElement(NULL);
    while (felem)
    {
      if (XMLNS::NameEqual(felem->Name(), "faultcode") && felem->GetText())
        vars.push_back(ElementPtr(new Element("faultcode", felem->GetText())));
      else if (XMLNS::NameEqual(felem->Name(), "faultstring") && felem->GetText())
        vars.push_back(ElementPtr(new Element("faultstring", felem->GetText())));
      else if (XMLNS::NameEqual(felem->Name(), "detail"))
      {
        const tinyxml2::XMLElement* delem = felem->FirstChildElement(NULL);
        if (delem && (delem = delem->FirstChildElement(NULL)))
        {
          do
          {
            if (delem->GetText())
            {
              // remove the namespace qualifier to handle local name as key
              vars.push_back(ElementPtr(new Element(XMLNS::LocalName(delem->Name()), delem->GetText())));
              DBG(DBG_PROTO, "%s: [fault] %s = %s\n", __FUNCTION__, vars.back()->GetKey().c_str(), vars.back()->c_str());
            }
            delem = delem->NextSiblingElement(NULL);
          } while (delem);
        }
      }
      felem = felem->NextSiblingElement(NULL);
    }
    SetFault(vars);
  }
  else
  {
    elem = elem->FirstChildElement(NULL);
    while (elem)
    {
      if (elem->GetText())
      {
        vars.push_back(ElementPtr(new Element(SMAPIDict.TranslateQName(xmlnames, elem->Name()), elem->GetText())));
        DBG(DBG_PROTO, "%s: %s = %s\n", __FUNCTION__, vars.back()->GetKey().c_str(), vars.back()->c_str());
      }
      else if (!elem->NoChildren())
      {
        tinyxml2::XMLPrinter out;
        elem->Accept(&out);
        vars.push_back(ElementPtr(new Element(SMAPIDict.TranslateQName(xmlnames, elem->Name()), out.CStr())));
        DBG(DBG_PROTO, "%s: dump (%s)\n%s\n", __FUNCTION__, vars.back()->GetKey().c_str(), vars.back()->c_str());
      }
      elem = elem->NextSiblingElement(NULL);
    }
  }
  return vars;
}

void SMAPI::SetFault(const ElementList& vars)
{
  OS::CLockGuard lock(*m_mutex);
  m_fault = vars;
  for (ElementList::const_iterator it = vars.begin(); it != vars.end(); ++it)
    DBG(DBG_ERROR, "%s: %s (%s)\n", __FUNCTION__, (*it)->GetKey().c_str(), (*it)->c_str());
}

ElementList SMAPI::Request(const std::string& action, const ElementList& args)
{
  ElementList vars = DoCall(action, args);

  // Rebuild the soap header using fresh token which is filled in fault
  if (vars.GetValue("TAG") == "Fault" && vars.GetValue("faultstring") == "TokenRefreshRequired")
  {
    /*
     <s:Fault>
     <faultcode>s:Client.TokenRefreshRequired</faultcode>
     <faultstring>TokenRefreshRequired</faultstring>
     <detail>
     <ns:refreshAuthTokenResult>
     <ns:authToken>NEW_TOKEN</ns:authToken>
     <ns:privateKey>REFRESH_TOKEN</ns:privateKey>
     </ns:refreshAuthTokenResult>
     </detail>
     </s:Fault>
    */
    SMAccount::OACredentials cr = m_service->GetAccount()->GetOACredentials();
    cr.token = vars.GetValue("authToken");
    cr.key = vars.GetValue("privateKey");
    m_service->GetAccount()->SetOACredentials(cr);
    makeSoapHeader();
    // Retry the request
    vars = DoCall(action, args);
  }
  return vars;
}