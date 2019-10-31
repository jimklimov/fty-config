/*  =========================================================================
    fty_config_manager - Fty config manager

    Copyright (C) 2014 - 2018 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
 */

/*
@header
    fty_config_manager - Fty config manager
@discuss
@end
 */

#include <iostream>
#include <sstream>
#include <vector>
#include <list>

#include "fty_config_classes.h"


using namespace std::placeholders;

namespace config
{   
    /**
     * Constructor
     * @param parameters
     * @param streamPublisher
     */
    ConfigurationManager::ConfigurationManager(const std::map<std::string, std::string> & parameters)
    : m_parameters(parameters), m_aug(NULL)
    {
        init();
    }

    /**
     * Destructor
     */
    ConfigurationManager::~ConfigurationManager()
    {
        aug_close(m_aug);
        delete m_msgBus;
        log_debug("All resources released");
    }

    /**
     * Initialization class
     */
    void ConfigurationManager::init()
    {
        try
        {
            // Augeas tool init
            int augeasOpt = getAugeasFlags(m_parameters.at(AUGEAS_OPTIONS));
            log_debug("augeas options: %d", augeasOpt);

            m_aug = aug_init(FILE_SEPARATOR, m_parameters.at(AUGEAS_LENS_PATH).c_str(), augeasOpt);
            if (!m_aug)
            {
                throw ConfigurationException("Augeas tool initialization failed");
            }

            // Message bus init
            m_msgBus = messagebus::MlmMessageBus(m_parameters.at(ENDPOINT_KEY), m_parameters.at(AGENT_NAME_KEY));
            m_msgBus->connect();
            
            // Listen all incoming request
            auto fct = std::bind(&ConfigurationManager::handleRequest, this, _1);
            m_msgBus->receive(m_parameters.at(QUEUE_NAME_KEY), fct);
        }
        catch (messagebus::MessageBusException& ex)
        {
            log_error("Message bus error: %s", ex.what());
        } catch (...)
        {
            log_error("Unexpected error: unknown");
        }
    }

    /**
     * Handle any request
     * @param sender
     * @param payload
     * @return 
     */
    void ConfigurationManager::handleRequest(messagebus::Message msg)
    {
        try
        {
            log_debug("Configuration handle request");
            
            dto::UserData userData;
            // Get request
            dto::UserData data = msg.userData();
            dto::config::ConfigQueryDto configQuery;
            data >> configQuery;

            log_debug("Config query-> action: %s", configQuery.action.c_str());
            if (configQuery.action.empty())
            {
                if ((configQuery.action == SAVE_ACTION && configQuery.features.empty()) ||
                    (configQuery.action ==  RESTORE_ACTION && configQuery.data.empty()))
                {
                    throw ConfigurationException("Request not valid");
                }
            }
            // Load augeas for any request (to avoid any cache).
            aug_load(m_aug);
            
            // Check if the command is implemented
            if (configQuery.action == SAVE_ACTION)
            {
                // Response
                dto::config::ConfigResponseDto respDto(""/*configQuery.featureName*/, STATUS_FAILED);
                std::map<std::string, cxxtools::SerializationInfo> configSiList;
                for(auto const& feature: configQuery.features)
                {
                    // Get the configuration file path name from class variable m_parameters
                    std::string configurationFileName = AUGEAS_FILES + m_parameters.at(feature) + ANY_NODES;
                    log_debug("Configuration file name: %s", configurationFileName.c_str());
                    
                    cxxtools::SerializationInfo si;
                    getConfigurationToJson(si, configurationFileName);
                    configSiList[feature] = si;
                }
                // Set response
                setSaveResponse(configSiList, respDto);
                userData << respDto;
            } 
            else if (configQuery.action == RESTORE_ACTION)
            {
                // To store response
                dto::srr::SrrRestoreDtoList srrRestoreDtoList(STATUS_SUCCESS); 
                // Get request and serialize it
                cxxtools::SerializationInfo restoreSi;
                JSON::readFromString(configQuery.data, restoreSi);
                // Get data member
                cxxtools::SerializationInfo siData = restoreSi.getMember(DATA_MEMBER);
                cxxtools::SerializationInfo::Iterator it;
                for (it = siData.begin(); it != siData.end(); ++it)
                {
                    dto::srr::SrrRestoreDto respDto(it->begin()->name(), STATUS_FAILED);
                    // Build the augeas configuration file name.
                    std::string configurationFileName = AUGEAS_FILES + m_parameters.at(respDto.name);
                    log_debug("Restore configuration for: %s, with configuration file: %s", respDto.name.c_str(), configurationFileName.c_str());
                    
                    cxxtools::SerializationInfo siData = it->getMember(respDto.name).getMember(DATA_MEMBER);
                    int returnValue = setConfiguration(&siData, configurationFileName);
                    if (returnValue == 0)
                    {
                        log_debug("Restore configuration for: %s succeed!", respDto.name.c_str());
                        respDto.status = STATUS_SUCCESS;
                    } 
                    else
                    {
                        std::string errorMsg = "Restore configuration for: " + respDto.name + " failed!";
                        log_error(errorMsg.c_str());
                        respDto.error = errorMsg;
                        srrRestoreDtoList.status = STATUS_FAILED;
                    }
                    srrRestoreDtoList.responseList.push_back(respDto);
               }
               userData << srrRestoreDtoList;
            } 
            else
            {
                throw ConfigurationException("Wrong command");
            }
            // Send response
            sendResponse(msg, userData, configQuery.action);
        } catch (const std::out_of_range& oor)
        {
            log_error("Feature name not found");
        } catch (std::exception &e)
        {
            log_error("Unexpected error: %s", e.what());
        } catch (...)
        {
            log_error("Unexpected error: unknown");
        }
    }
    
    /**
     * Set response before to sent it.
     * @param msg
     * @param responseDto
     * @param configQuery
     */
    void ConfigurationManager::setSaveResponse (const std::map<std::string, cxxtools::SerializationInfo>& configSiList, dto::config::ConfigResponseDto& respDto)
    {
        // Array si
        cxxtools::SerializationInfo jsonResp;
        jsonResp.setCategory(cxxtools::SerializationInfo::Category::Array);

        for(auto const& configSi: configSiList)
        {
            cxxtools::SerializationInfo si;
            si.setCategory(cxxtools::SerializationInfo::Category::Object);
            // Feature si
            cxxtools::SerializationInfo siFeature;
            // Content si
            cxxtools::SerializationInfo siTemp;
            siTemp.addMember(SRR_VERSION) <<= ACTIVE_VERSION;
            cxxtools::SerializationInfo& siData = siTemp.addMember(DATA_MEMBER);
            siData <<= configSi.second;
            siData.setName(DATA_MEMBER);
            // Add si version + data in si feature
            siFeature <<= siTemp;
            siFeature.setName(configSi.first);
            // Add the feature in the main si
            si.addMember(configSi.first) <<= siFeature;
            // Put in the array
            jsonResp.addMember(configSi.first) <<= si;
        }
        // Serialize the response
        respDto.data = JSON::writeToString (jsonResp, false);
        respDto.status = STATUS_SUCCESS;
    }
    
    /**
     * Send response on message bus.
     * @param msg
     * @param responseDto
     * @param configQuery
     */
    void ConfigurationManager::sendResponse(const messagebus::Message& msg, const dto::UserData& userData, const std::string& subject)
    {
        try
        {
            messagebus::Message resp;
            resp.userData() = userData;
            resp.metaData().emplace(messagebus::Message::SUBJECT, subject);
            resp.metaData().emplace(messagebus::Message::FROM, m_parameters.at(AGENT_NAME_KEY));
            resp.metaData().emplace(messagebus::Message::TO, msg.metaData().find(messagebus::Message::FROM)->second);
            resp.metaData().emplace(messagebus::Message::COORELATION_ID, msg.metaData().find(messagebus::Message::COORELATION_ID)->second);
            m_msgBus->sendReply(msg.metaData().find(messagebus::Message::REPLY_TO)->second, resp);
        }
        catch (messagebus::MessageBusException& ex)
        {
            log_error("Message bus error: %s", ex.what());
        } catch (...)
        {
            log_error("Unexpected error: unknown");
        }
    }

    /**
     * Set a configuration.
     * @param si
     * @param path
     * @return 
     */
    int ConfigurationManager::setConfiguration(cxxtools::SerializationInfo* si, const std::string& path)
    {
        cxxtools::SerializationInfo::Iterator it;
        for (it = si->begin(); it != si->end(); ++it)
        {
            cxxtools::SerializationInfo *member = &(*it);
            std::string memberName = member->name();

            cxxtools::SerializationInfo::Iterator itElement;
            for (itElement = member->begin(); itElement != member->end(); ++itElement)
            {
                cxxtools::SerializationInfo *element = &(*itElement);
                std::string elementName = element->name();
                std::string elementValue;
                element->getValue(elementValue);
                // Build augeas full path
                std::string fullPath = path + FILE_SEPARATOR + memberName + FILE_SEPARATOR + elementName;
                // Set value
                int setReturn = aug_set(m_aug, fullPath.c_str(), elementValue.c_str());
            }
        }
        return aug_save(m_aug);
    }

    /**
     * Get a configuration serialized to json format.
     * @param path
     */
    void ConfigurationManager::getConfigurationToJson(cxxtools::SerializationInfo& si, std::string& path)
    {
        std::cout << "path " << path << std::endl; 
        char **matches;
        int nmatches = aug_match(m_aug, path.c_str(), &matches); 

        // no matches, stop it.
        if (nmatches < 0) return;

        // Iterate on all matches
        for (int i = 0; i < nmatches; i++)
        {
            std::string temp = matches[i];
            
            // Skip all comments
            if (temp.find(COMMENTS_DELIMITER) == std::string::npos)
            {
                const char *value, *label;
                aug_get(m_aug, matches[i], &value);
                aug_label(m_aug, matches[i], &label);
                if (!value)
                {
                    // It's a member
                    si.addMember(label);
                } 
                else
                {
                    std::string t = findMemberFromMatch(temp);
                    cxxtools::SerializationInfo *siTemp = si.findMember(t);
                    if (siTemp)
                    {
                        siTemp->addMember(label) <<= value;
                    }
                }
                getConfigurationToJson(si, temp.append(ANY_NODES));
            }
        }
    }

    /**
     * Find a member
     * @param input
     * @return siTemp
     */
    std::string ConfigurationManager::findMemberFromMatch(const std::string& input)
    {
        std::string returnValue = "";
        if (input.length() > 0)
        {
            // Try to find last /
            std::size_t found = input.find_last_of(FILE_SEPARATOR);
            if (found != -1)
            {
                std::string temp = input.substr(0, found);
                found = temp.find_last_of(FILE_SEPARATOR);
                returnValue = temp.substr(found + 1, temp.length());
            }
        }
        return returnValue;
    }

    /**
     * Utilitary to dump a configuration.
     * @param path
     */
    void ConfigurationManager::dumpConfiguration(std::string& path)
    {
        char **matches;
        int nmatches = aug_match(m_aug, path.c_str(), &matches);

        // Stop if not matches.
        if (nmatches < 0) return;

        // Iterate on all matches
        for (int i = 0; i < nmatches; i++)
        {
            std::string temp = matches[i];
            // Skip all comments
            if (temp.find(COMMENTS_DELIMITER) == std::string::npos)
            {
                const char *value, *label;
                aug_get(m_aug, matches[i], &value);
                aug_label(m_aug, matches[i], &label);
                dumpConfiguration(temp.append(ANY_NODES));
            }
        }
    }

    /**
     * Get augeas tool flag
     * @param augeasOpts
     * @return 
     */
    int ConfigurationManager::getAugeasFlags(std::string& augeasOpts)
    {
        int returnValue = AUG_NONE;
        // Build static augeas option
        static std::map<const std::string, aug_flags> augFlags;
        augFlags["AUG_NONE"] = AUG_NONE;
        augFlags["AUG_SAVE_BACKUP"] = AUG_SAVE_BACKUP;
        augFlags["AUG_SAVE_NEWFILE"] = AUG_SAVE_NEWFILE;
        augFlags["AUG_TYPE_CHECK"] = AUG_TYPE_CHECK;
        augFlags["AUG_NO_STDINC"] = AUG_NO_STDINC;
        augFlags["AUG_SAVE_NOOP"] = AUG_SAVE_NOOP;
        augFlags["AUG_NO_LOAD"] = AUG_NO_LOAD;
        augFlags["AUG_NO_MODL_AUTOLOAD"] = AUG_NO_MODL_AUTOLOAD;
        augFlags["AUG_ENABLE_SPAN"] = AUG_ENABLE_SPAN;
        augFlags["AUG_NO_ERR_CLOSE"] = AUG_NO_ERR_CLOSE;
        augFlags["AUG_TRACE_MODULE_LOADING"] = AUG_TRACE_MODULE_LOADING;
        
        if (augeasOpts.size() > 1 )
        {
            // Replace '|' by ' '
            std::replace(augeasOpts.begin(), augeasOpts.end(), '|', ' ');

            // Build all options parameters in array
            std::vector<std::string> augOptsArray;
            std::stringstream ss(augeasOpts);
            std::string temp;
            while (ss >> temp)
            {
                augOptsArray.push_back(temp);
            }

            // Builds augeas options
            std::vector<std::string>::iterator it;
            for (it = augOptsArray.begin(); it != augOptsArray.end(); ++it)
            {
                returnValue |= augFlags.at(*it);
            }
        }
        return returnValue;
    }
}