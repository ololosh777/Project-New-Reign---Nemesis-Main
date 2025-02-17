#include <atomic>
#include <condition_variable>

#include "debuglog.h"
#include "nemesisinfo.h"

#include "ui/Terminator.h"

#include "utilities/conditions.h"
#include "utilities/threadpool.h"
#include "utilities/atomiclock.h"
#include "utilities/stringsplit.h"
#include "utilities/readtextfile.h"

#include "generate/addanims.h"
#include "generate/addevents.h"
#include "generate/addvariables.h"
#include "generate/behaviorprocess.h"
#include "generate/playerexclusive.h"
#include "generate/generator_utility.h"
#include "generate/behaviorgenerator.h"
#include "generate/behaviorsubprocess.h"
#include "generate/animationdatatracker.h"
#include "generate/behaviorprocess_utility.h"

#include "generate/animation/import.h"
#include "generate/animation/nodejoint.h"
#include "generate/animation/templatetree.h"
#include "generate/animation/grouptemplate.h"
#include "generate/animation/singletemplate.h"
#include "generate/animation/registeranimation.h"
#include "generate/animation/templateprocessing.h"

using namespace std;
namespace sf = filesystem;

struct NodeJoint;

extern Terminator* p_terminate;
extern atomic<int> m_RunningThread;
extern atomic_flag animdata_lock;

atomic<int> extraCore = 0;
atomic<int> behaviorRun;
condition_variable cv2;
mutex cv2_m;

struct IDCatcher
{
private:
    int ID;
    int line;

public:
    IDCatcher(int id, int curline)
    {
        ID   = id;
        line = curline;
    }

    int getID()
    {
        return ID;
    }

    int getLine()
    {
        return line;
    }
};

struct SubEnd
{
    BehaviorStart* process;

    SubEnd(BehaviorStart* _process)
    {
        process = _process;
    };

    ~SubEnd()
    {
        {
            lock_guard<mutex> lg(cv2_m);
            --behaviorRun;
        }

        cv2.notify_one();
        process->EndAttempt();
    }
};

BehaviorSub::BehaviorSub(const NemesisInfo* _ini)
{
    nemesisInfo = _ini;
}

void BehaviorSub::BehaviorCompilation()
{
    SubEnd trigger(process);

    try
    {
        ++m_RunningThread;

        {
            lock_guard<mutex> lg(cv2_m);
            ++behaviorRun;
        }

        try
        {
            try
            {
                CompilingBehavior();
            }
            catch (exception& ex)
            {
                ErrorMessage(6002, curfilefromlist, ex.what());
            }
        }
        catch (nemesis::exception&)
        {
            // resolved exception
        }
    }
    catch (...)
    {
        try
        {
            ErrorMessage(6002, curfilefromlist, "Unknown");
        }
        catch (nemesis::exception&)
        {
            // resolved exception
        }
    }
}

void BehaviorSub::modPick(unordered_map<string, vector<pair<uint, shared_ptr<string>>>>& modEditStore,
                          vector<pair<uint, string>>& catalyst,
                          vector<pair<uint, string>>& modLine,
                          bool& hasDeleted)
{
    if (!modPickProcess(modEditStore, catalyst, modLine, hasDeleted))
    {
        for (auto& line : modEditStore["current"])
        {
            modLine.push_back(make_pair(line.first, ""));
            catalyst.push_back(make_pair(line.first, *line.second));
        }
    }

    modEditStore.clear();
    hasDeleted = false;
}

bool BehaviorSub::modPickProcess(unordered_map<string, vector<pair<uint, shared_ptr<string>>>>& modEditStore,
                                 vector<pair<uint, string>>& catalyst,
                                 vector<pair<uint, string>>& modLine,
                                 bool& hasDeleted)
{
    if (modEditStore.size() > 0) return false;

    auto& orig = modEditStore["original"];

    if (hasDeleted || orig.size() <= 2) return false;

    string templine = nemesis::regex_replace(*orig[0].second, nemesis::regex("^([\t]+).*$"), string("\\1"));
    int counter     = count(templine.begin(), templine.end(), '\t');

    string lastline
        = nemesis::regex_replace(*orig.back().second, nemesis::regex("^([\t]+).*$"), string("\\1"));
    int counter2    = count(lastline.begin(), lastline.end(), '\t');

    if (counter != counter2) return false;

    vector<size_t> elePoint;

    for (unsigned int i = 0; i < orig.size(); ++i)
    {
        string templine
            = nemesis::regex_replace(*orig[i].second, nemesis::regex("^([\t]+).*$"), string("\\1"));

        if (*orig[i].second != templine)
        {
            counter2 = count(templine.begin(), templine.end(), '\t');

            if (counter == counter2) elePoint.push_back(i);
        }
    }

    elePoint.push_back(orig.size());
    vector<pair<uint, string>> storeline;
    storeline.reserve(modEditStore["current"].size());

    for (unsigned int i = 0; i < elePoint.back() - 1; ++i)
    {
        bool done      = false;
        unsigned int e = elePoint[i];

        for (const string& mod : process->behaviorPriority)
        {
            if (modEditStore[mod][e].second != nullptr)
            {
                do
                {
                    auto& ref = modEditStore[mod][e];
                    storeline.push_back(make_pair(ref.first, *ref.second));
                } while (++e < elePoint[i + 1]);

                done = true;
                break;
            }
        }

        if (!done)
        {
            do
            {
                auto& ref = modEditStore["current"][e];
                storeline.push_back(make_pair(ref.first, *ref.second));
            } while (++e < elePoint[i + 1]);
        }
    }

    int g = 0;

    for (unsigned int i = 0; i < storeline.size(); ++i)
    {
        if (storeline[i].second != *modEditStore["current"][i].second)
        {
            interMsg("test");
            g++;
        }
    }

    if (g == 1)
    {
        for (auto& line : storeline)
        {
            modLine.push_back(make_pair(line.first, ""));
        }

        catalyst.insert(catalyst.end(), storeline.begin(), storeline.end());
    }

    return true;
}

void BehaviorSub::CompilingBehavior()
{
    namespace sf = std::filesystem;
    ImportContainer exportID;

    wstring filepath         = directory + nemesis::transform_to<wstring>(curfilefromlist);
    string behaviorFile      = curfilefromlist.substr(0, curfilefromlist.find_last_of("."));
    string lowerBehaviorFile = nemesis::to_lower_copy(nemesis::transform_to<string>(behaviorFile));

    bool isFirstPerson = lowerBehaviorFile.find("_1stperson") != NOT_FOUND;

    int lastID  = 0;
    int curID   = 0;
    int firstID = -1;

    unordered_map<string, bool> orievent;
    unordered_map<string, bool> orivariable;

    ID eventid;
    ID variableid;
    ID attriid;
    ID charid;

    unordered_map<int, unordered_map<string, VecStr>>
        clipAA; // node id, original animation, list of alternative animations path
    unordered_map<string, vector<IDCatcher>> catcher;

    SSMap IDExist;
    map<int, VecStr> catalystMap;
    VecStr characterFiles;

    set<string> pceaMod;
    unordered_map<int, vector<PCEAData>*> pceaID; // node ID, list of mods

    // final output
    sf::path outputdir;
    string ZeroEvent;
    string ZeroVariable;

#ifdef DEBUG
    if (modID.length() > 0)
    {
        outputdir = "new_behaviors\\"
                    + GetFileDirectory(behaviorPath[lowerBehaviorFile].substr(
                        behaviorPath[lowerBehaviorFile].find("\\") + 1))
                    + modID + lowerBehaviorFile;
    }
    else
    {
        outputdir = "new_behaviors\\"
                    + behaviorPath[lowerBehaviorFile].substr(behaviorPath[lowerBehaviorFile].find("\\") + 1);
    }
#else
    if (modID.length() > 0)
    {
        outputdir = GetFileDirectory(behaviorPath[nemesis::transform_to<wstring>(lowerBehaviorFile)]).data()
                    + nemesis::transform_to<wstring>(modID)
                    + nemesis::transform_to<wstring>(lowerBehaviorFile);
    }
    else
    {
        outputdir = behaviorPath[nemesis::transform_to<wstring>(lowerBehaviorFile)];
    }
#endif
    if (error) throw nemesis::exception();

    DebugLogging(L"Processing behavior: " + filepath);
    DebugLogging(L"Behavior output path: " + outputdir.wstring());
    process->newMilestone();

    if (modID.length() > 0 && isFileExist(outputdir.wstring() + L".hkx"))
    {
        int i = 0;

        while (i < 9)
        {
            process->newMilestone();
            ++i;
        }

        return;
    }

    double duration;

    {
        bool hasAA   = alternateAnim.size() != 0;
        bool isOpen  = true;
        bool newBone = false;
        bool modif   = false;
        bool orig    = false;

        int counter           = 0;
        int oribone           = -1;
        int bonenum           = -1;
        int eventelements     = -1;
        int variableelements  = -1;
        int attributeelements = -1;
        int characterelements = -1;
        size_t elementLine    = -1;
        size_t openRange      = 0;

        unordered_map<int, string> eventName;
        unordered_map<int, string> varName;
        unordered_map<int, string> attriName;
        unordered_map<int, string> charName;

        unordered_map<string, string> chosenLines;
        unordered_map<string, VecStr> chosenVecLines;

        string newMod;

        set<string> AAGroupList;
        set<string> AAEventName;

        // read behavior file
        vector<pair<uint, string>> catalyst;
        vector<pair<uint, string>> modLine;
        VecStr origLines;

        if (sf::is_directory(filepath)) ErrorMessage(3001, filepath);

        size_t size = fileLineCount(filepath);
        catalyst.reserve(size);
        modLine.reserve(size);
        FileReader BehaviorFormat(filepath);

        if (!BehaviorFormat.GetFile()) ErrorMessage(3002, filepath);

        bool hasDeleted = false;
        uint numline    = 0;

        string line;
        unordered_map<string, vector<pair<uint, shared_ptr<string>>>> modEditStore;

        auto storingLine = [&]() 
        {
            if (line.find("<!-- *", 0) != NOT_FOUND)
            {
                size_t tempint   = line.find("<!-- *") + 6;
                string mod       = line.substr(tempint, line.find("* -->", tempint + 1) - tempint);
                chosenLines[mod] = line;
                modEditStore[mod].push_back(make_pair(numline, make_shared<string>(line)));

                if (!hasDeleted
                    && line
                           == nemesis::regex_replace(
                               line, nemesis::regex("^[\t]+<!-- \\*([\\w]+)\\* -->"), string("\\1")))
                    hasDeleted = true;
            }
            else if (line.find("\t<!-- original -->", 0) != NOT_FOUND)
            {
                modEditStore["original"].push_back(make_pair(numline, make_shared<string>(line)));

                if (chosenLines.size() == 0) ErrorMessage(1209);

                line = behaviorLineChooser(line, chosenLines, process->behaviorPriority);
                chosenLines.clear();
                modEditStore["current"].push_back(make_pair(numline, make_shared<string>(line)));

                for (auto& each : process->behaviorPriority)
                {
                    if (modEditStore[each].size() != modEditStore["original"].size())
                    {
                        modEditStore[each].push_back(make_pair(numline, nullptr));
                    }
                }
            }
            else
            {
                size_t pos = line.find("<hkobject name=\"");

                if (pos != NOT_FOUND && line.find("signature=\"", pos) != NOT_FOUND)
                {
                    string ID = nemesis::regex_replace(
                        string(line),
                        nemesis::regex(".*<hkobject name=\"#([0-9]+)\" class=\"[a-zA-Z]+\" "
                                       "signature=\".*\">.*"),
                        string("\\1"));

                    if (ID != line)
                    {
                        curID = stoi(ID);

                        if (curID >= lastID) lastID = curID + 1;
                    }
                }

                modLine.push_back(make_pair(numline, newMod));
                catalyst.push_back(make_pair(numline, line));
            }
        };

        while (BehaviorFormat.GetLines(line))
        {
            bool skip = false;

            if (line.find("<!-- ") != NOT_FOUND)
            {
                if (line.find("\t\t<!-- *") == NOT_FOUND && line.find("\t\t<!-- original -->") && NOT_FOUND)
                {
                    modPick(modEditStore, catalyst, modLine, hasDeleted);
                }

                if (line.find("<!-- NEW *") != NOT_FOUND)
                {
                    size_t tempint = line.find("<!-- NEW *") + 10;
                    string mod     = line.substr(tempint, line.find("* -->", tempint + 1) - tempint);

                    if (process->chosenBehavior.find(mod) == process->chosenBehavior.end())
                    {
                        isOpen = false;
                    }
                    else
                    {
                        newMod = mod;
                        isOpen = true;
                    }

                    modif = true;
                    skip  = true;
                }
                else if (line.find("<!-- ORIGINAL -->") != NOT_FOUND)
                {
                    if (!modif) ErrorMessage(1211, filepath, numline);

                    isOpen = true;
                    orig   = true;
                    skip   = true;
                }
                else if (line.find("<!-- CLOSE -->") != NOT_FOUND)
                {
                    if (modif)
                    {
                        for (auto& bhv : process->behaviorPriority)
                        {
                            auto itr = chosenVecLines.find(bhv);

                            if (itr != chosenVecLines.end())
                            {
                                newMod = bhv;
                                modif  = false;

                                for (auto& eachline : itr->second)
                                {
                                    line = eachline;
                                    storingLine();
                                }

                                break;
                            }
                        }

                        if (modif)
                        {
                            for (auto& eachline : origLines)
                            {
                                line = eachline;
                                storingLine();
                            }
                        }

                        chosenVecLines.clear();
                        origLines.clear();
                        newMod.clear();
                        skip   = true;
                        modif  = false;
                        isOpen = true;
                    }
                }
            }
            else
            {
                modPick(modEditStore, catalyst, modLine, hasDeleted);
            }

            if (isOpen && !skip)
            {
                if (line == "	</hksection>") break;

                if (modif)
                {
                    if (orig)
                    {
                        origLines.push_back(line);
                        continue;
                    }

                    chosenVecLines[newMod].push_back(line);
                    continue;
                }

                storingLine();
            }

            if (error) throw nemesis::exception();
        }

        if (catalyst.size() != 0 && catalyst.back().second.length() != 0
            && catalyst.back().second.find("<!-- CONDITION END -->") == NOT_FOUND
            && catalyst.back().second.find("<!-- CLOSE -->") == NOT_FOUND)
        {
            modLine.push_back(make_pair(modLine.size(), newMod));
            catalyst.push_back(make_pair(catalyst.size(),""));
        }

        DebugLogging(L"Processing behavior: " + filepath
                     + L" (Check point 1, File extraction & mod selection complete)");

        if (isCharacter)
        {
            DebugLogging(L"Processing behavior: " + filepath + L" (IsCharater: TRUE)");
            string rigfile = "<hkparam name=\"rigName\">";
            string bonemap = "<hkparam name=\"bonePairMap\" numelements=\"";
            bool found     = false;

            for (auto& linkedline : catalyst)
            {
                auto& line = linkedline.second;

                if (line.find(bonemap) != NOT_FOUND)
                {
                    size_t pos = line.find(bonemap) + bonemap.length();
                    oribone    = stoi(line.substr(pos, line.find("\">", pos) - pos));
                }
                else if (line.find(rigfile) != NOT_FOUND)
                {
                    size_t pos = line.find(rigfile) + rigfile.length();
                    rigfile    = line.substr(pos, line.find("</hkparam>", pos) - pos);
                    found      = true;
                    break;
                }
            }

            sf::path curFile(GetFileDirectory(outputdir));
            wstring wrigfile = curFile.parent_path().parent_path().wstring() + L"\\"
                               + nemesis::transform_to<wstring>(rigfile);
            
            if (found && isFileExist(wrigfile) && !sf::is_directory(wrigfile))
            {
                bonenum = bonePatch(wrigfile, oribone, newBone, process->hkxCompiler);
            }

            DebugLogging(L"Processing behavior: " + filepath
                         + L" (Check point 1.5, Character bone identification complete)");
        }
        else
        {
            characterFiles = behaviorJoints[nemesis::transform_to<string>(lowerBehaviorFile)];
            DebugLogging(L"Processing behavior: " + filepath + L" (IsCharater: FALSE)");
        }

        if (error) throw nemesis::exception();

        process->newMilestone();

        curID         = 0;
        bool isClip   = false;
        bool negative = false;
        string curNum;
        unordered_map<string, int> stateStrID; // parent node ID, new state ID counter; counter for state ID
        unordered_map<string, string>
            registeredStateID; // parent node ID & base ID & modcode & ID, existing state ID
        unordered_map<string, bool> isAdded;

        for (auto& linkedline : catalyst)
        {
            auto& line = linkedline.second;
            size_t pos = line.find("<hkobject name=\"", 0);

            if (pos != NOT_FOUND && line.find("signature=\"", 0) != NOT_FOUND)
            {
                pos += 17;
                string nodeID = line.substr(pos, line.find("\" class=\"", pos) - pos);

                if (isOnlyNumber(nodeID)) 
                {
                    curID = stoi(nodeID); 
                }
                else
                {
                    if (nodeID.find("$", 0) != NOT_FOUND)
                    {
                        string mod = nodeID.substr(0, nodeID.find("$"));

                        if (process->chosenBehavior.find(mod) != process->chosenBehavior.end())
                        {
                            string ID = to_string(lastID);

                            while (ID.length() < 4)
                            {
                                ID = "0" + ID;
                            }

                            line.replace(line.find(nodeID), nodeID.length(), ID);
                            IDExist[nodeID] = ID;
                            curID           = lastID;
                            ++lastID;

                            if (lastID == 9216) ++lastID;
                        }
                        else if (mod == "MID")
                        {
                            ErrorMessage(1020);
                        }
                    }
                }

                isClip = line.find("class=\"hkbClipGenerator\" signature=\"") != NOT_FOUND;
            }
            else if (isClip)
            {
                pos = line.find("<hkparam name=\"animationName\">");

                if (pos != NOT_FOUND)
                {
                    pos += 30;
                    string animPath
                        = nemesis::to_lower_copy(line.substr(pos, line.find("</hkparam>", pos) - pos));
                    addUsedAnim(lowerBehaviorFile, animPath);
                    string animFile = GetFileName(animPath) + ".hkx";

                    if (!isFirstPerson)
                    {
                        auto replacer = animReplaced.find(animFile);

                        if (replacer != animReplaced.end() && replacer->second.size())
                        {
                            pceaID.insert(make_pair(curID, &replacer->second));

                            for (auto& data : replacer->second)
                            {
                                pceaMod.insert(data.modFile);
                            }
                        }
                    }

                    // Dynamic AA function without a template
                    if (hasAA)
                    {
                        string tempAnimFile = isFirstPerson ? animFile + "_1p*" : animFile;
                        auto aa             = alternateAnim.find(tempAnimFile);

                        if (aa != alternateAnim.end())
                        {
                            clipAA[curID][animFile] = aa->second;
                            AAGroupList.insert(isFirstPerson ? AAGroup[animFile] + "_1p*"
                                                             : AAGroup[animFile]);
                            auto aaEvent = AAHasEvent.find(tempAnimFile);

                            if (aaEvent != AAHasEvent.end())
                            {
                                for (string& AA_animName : aaEvent->second)
                                {
                                    for (unsigned int k = 0; k < AAEvent[AA_animName].size(); ++k)
                                    {
                                        if (k % 2 == 0) AAEventName.insert(AAEvent[AA_animName][k]);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (error) throw nemesis::exception();
        }

        DebugLogging(L"Processing behavior: " + filepath
                     + L" (Check point 2, ID replacement & PCEA record complete)");

        {
            size_t pos = catalyst[1].second.find("toplevelobject=");

            if (pos == NOT_FOUND)
            {
                for (unsigned int l = 0; l < catalyst.size(); ++l)
                {
                    if (l > 50) break;

                    string_view vline(catalyst[l].second.c_str());

                    pos = vline.find("toplevelobject=");

                    if (pos != NOT_FOUND)
                    {
                        firstID = stoi(nemesis::regex_replace(
                            string(vline.substr(pos)), nemesis::regex("[^0-9]*([0-9]+).*"), string("\\1")));
                        break;
                    }
                }

                if (firstID == -1) ErrorMessage(1095, filepath);
            }
            else
            {
                firstID = stoi(nemesis::regex_replace(string(catalyst[1].second.substr(pos)),
                                                      nemesis::regex("[^0-9]*([0-9]+).*"),
                                                      string("\\1")));
            }
        }

        curID                  = firstID;

        int special            = 0;

        bool doneEventName     = false;
        bool doneEventInfo     = false;
        bool doneAttributeName = false;
        bool doneAttributeDef  = false;
        bool doneCharacProName = false;
        bool doneCharacProInfo = false;
        bool doneVarName       = false;
        bool doneVarWord       = false;
        bool doneVarInfo       = false;
        bool doneAnimName      = false;
        bool doneDeform        = false;
        bool doneRigid         = false;
        bool doneAnimFileName  = false;
        bool isClipTrigger     = false;
        bool replacedNum       = false;

        bool eventOpen     = false;
        bool varOpen       = false;
        bool attriOpen     = false;
        bool charOpen      = false;
        bool animOpen      = false;
        bool fp_animOpen   = false;
        bool otherAnimOpen = false;
        bool norElement    = false;
        bool characterAA   = false;

        // add picked behavior and remove not picked behavior
        for (uint l = 0; l < catalyst.size(); ++l)
        {
            bool elementCatch = false;
            string line       = catalyst[l].second;
            nemesis::smatch check;

            if (line.length() == 0)
            { 
                catalystMap[curID].push_back(line); 
            }
            else
            {
                newMod = modLine[l].second;

                if (line.find("<!-- ", 0) != NOT_FOUND)
                {
                    if (line.find("<!-- NEW ^", 0) != NOT_FOUND || line.find("<!-- FOREACH ^") != NOT_FOUND)
                    {
                        ++special;
                    }
                    else if (line.find("<!-- CLOSE -->", 0) != NOT_FOUND)
                    {
                        --special;
                    }
                }

                if (line.find("<hkobject name=\"", 0) != NOT_FOUND
                    && line.find("signature=\"", 0) != NOT_FOUND)
                {
                    isClipTrigger = false;
                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line,
                            match,
                            nemesis::regex(
                                "<hkobject name=\"#(.*)\" class=\"([a-zA-Z]+)\" signature=\".*\">")))
                    {
                        curID       = stoi(match[1]);
                        elementLine = -1;

                        if (match[2] == "hkbClipTriggerArray") isClipTrigger = true;
                    }
                }

                if (line.find("$", line.find("#")) != NOT_FOUND)
                {
                    if (line.find(">#") != NOT_FOUND)
                    {
                        size_t reference = count(line.begin(), line.end(), '#');
                        size_t nextpos   = 0;

                        for (size_t k = 0; k < reference; ++k)
                        {
                            nextpos      = line.find("#", nextpos) + 1;
                            string numID = nemesis::regex_replace(string(line.substr(nextpos)),
                                                                  nemesis::regex("[^0-9]*([0-9]+).*"),
                                                                  string("\\1"));
                            string ID    = line.substr(nextpos, line.find(numID) - nextpos + numID.length());

                            if (line.find(ID, 0) != NOT_FOUND && ID.find("$") != NOT_FOUND)
                            {
                                if (IDExist[ID].length() != 0)
                                {
                                    nemesis::smatch match;
                                    nemesis::regex_match(line,
                                                         match,
                                                         nemesis::regex(".*#(" + ID.substr(0, ID.find("$"))
                                                                        + "[$]" + numID + ")[^0-9]*.*"));
                                    nextpos = match.position(1);
                                    line.replace(nextpos, ID.length(), IDExist[ID]);
                                }
                                else if (special == 0)
                                {
                                    IDCatcher catchingID(curID, int(catalystMap[curID].size()));
                                    catcher[ID].push_back(catchingID);
                                }
                            }
                        }
                    }
                    else if (line.find("\t\t\t#") != NOT_FOUND)
                    {
                        VecStr generator;
                        StringSplit(line, generator);
                        line.append("%");

                        for (unsigned int p = 0; p < generator.size(); p++)
                        {
                            string ID    = generator[p];
                            string numID = nemesis::regex_replace(
                                string(ID), nemesis::regex("[^0-9]*([0-9]+).*"), string("\\1"));

                            if (ID.find("$") != NOT_FOUND)
                            {
                                string masterFormat = ID.substr(1, ID.find("$") - 1);

                                if (ID == "#" + masterFormat + "$" + numID && line.find(ID, 0) != NOT_FOUND)
                                {
                                    ID = ID.substr(1, ID.length() - 1);

                                    if (IDExist[ID].length() != 0)
                                    {
                                        nemesis::smatch match;
                                        nemesis::regex_match(line,
                                                             match,
                                                             nemesis::regex(".*#(" + masterFormat + "[$]"
                                                                            + numID + ")[^0-9]+.*"));
                                        line.replace(match.position(1), ID.length(), IDExist[ID]);
                                    }
                                    else if (special == 0)
                                    {
                                        IDCatcher catchingID(curID, int(catalystMap[curID].size()));
                                        catcher[ID].push_back(catchingID);
                                    }
                                }
                            }
                        }

                        line.pop_back();
                    }
                }

                // get event id from newAnimation
                if ((!doneEventName && line.find("<hkparam name=\"eventNames\" numelements=", 0) != NOT_FOUND)
                    || (!doneEventInfo
                        && line.find("<hkparam name=\"eventInfos\" numelements=", 0) != NOT_FOUND))
                {
                    line.find("<hkparam name=\"eventNames\" numelements=", 0) != NOT_FOUND
                        ? doneEventName = true
                        : doneEventInfo = true;

                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line, match, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\"(.+?)\">")))
                    {
                        if (eventelements == -1)
                        {
                            if (line.find("</hkparam>") == NOT_FOUND)
                            {
                                counter         = 0;
                                eventOpen       = true;
                                elementCatch    = true;
                                string templine = line.substr(0, match.position(1));
                                openRange       = count(templine.begin(), templine.end(), '\t');
                                curNum          = match[1];
                            }
                        }
                        else
                        {
                            string templine = line.substr(0, match.position(1));
                            openRange       = count(templine.begin(), templine.end(), '\t');
                            curNum          = match[1];
                            replacedNum     = true;
                            eventOpen       = true;
                            line.replace(match.position(2), match[2].length(), to_string(eventelements));
                        }
                    }
                }
                else if ((!doneAttributeName
                          && line.find("<hkparam name=\"attributeNames\" numelements=") != NOT_FOUND)
                         || (!doneAttributeDef
                             && line.find("<hkparam name=\"attributeDefaults\" numelements=") != NOT_FOUND))
                {
                    line.find("<hkparam name=\"attributeNames\" numelements=") != NOT_FOUND
                        ? doneAttributeName = true
                        : doneAttributeDef  = true;

                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line, match, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\"(.+?)\">")))
                    {
                        if (attributeelements == -1)
                        {
                            if (line.find("</hkparam>") == NOT_FOUND)
                            {
                                counter         = 0;
                                attriOpen       = true;
                                elementCatch    = true;
                                string templine = line.substr(0, match.position(1));
                                openRange       = count(templine.begin(), templine.end(), '\t');
                                curNum          = match[1];
                            }
                        }
                        else
                        {
                            line.replace(match.position(2), match[2].length(), to_string(attributeelements));
                        }
                    }
                }
                else if ((!doneCharacProName
                          && line.find("<hkparam name=\"characterPropertyNames\" numelements=") != NOT_FOUND)
                         || (!doneCharacProInfo
                             && line.find("<hkparam name=\"characterPropertyInfos\" numelements=")
                                    != NOT_FOUND))
                {
                    line.find("<hkparam name=\"characterPropertyNames\" numelements=") != NOT_FOUND
                        ? doneCharacProName = true
                        : doneCharacProInfo = true;

                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line, match, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\"(.+?)\">")))
                    {
                        if (characterelements == -1)
                        {
                            if (line.find("</hkparam>") == NOT_FOUND)
                            {
                                counter         = 0;
                                charOpen        = true;
                                elementCatch    = true;
                                string templine = line.substr(0, match.position(1));
                                openRange       = count(templine.begin(), templine.end(), '\t');
                                curNum          = match[1];
                            }
                        }
                        else
                        {
                            line.replace(match.position(2), match[2].length(), to_string(characterelements));
                        }
                    }
                }
                else if ((!doneVarName
                          && line.find("<hkparam name=\"variableNames\" numelements=") != NOT_FOUND)
                         || (!doneVarWord
                             && line.find("<hkparam name=\"wordVariableValues\" numelements=") != NOT_FOUND)
                         || (!doneVarInfo
                             && line.find("<hkparam name=\"variableInfos\" numelements=") != NOT_FOUND))
                {
                    line.find("<hkparam name=\"variableNames\" numelements=") != NOT_FOUND
                        ? doneVarName = true
                        : line.find("<hkparam name=\"wordVariableValues\" numelements=") != NOT_FOUND
                              ? doneVarWord = true
                              : doneVarInfo = true;

                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line, match, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\"(.+?)\">")))
                    {
                        if (variableelements == -1)
                        {
                            if (line.find("</hkparam>") == NOT_FOUND)
                            {
                                counter         = 0;
                                varOpen         = true;
                                elementCatch    = true;
                                string templine = line.substr(0, match.position(1));
                                openRange       = count(templine.begin(), templine.end(), '\t');
                                curNum          = match[1];
                            }
                        }
                        else
                        {
                            replacedNum     = true;
                            varOpen         = true;
                            string templine = line.substr(0, match.position(1));
                            openRange       = count(templine.begin(), templine.end(), '\t');
                            curNum          = match[1];
                            line.replace(match.position(2), match[2].length(), to_string(variableelements));
                        }
                    }
                }
                else if (!doneAnimName
                         && line.find("<hkparam name=\"animationNames\" numelements=\"") != NOT_FOUND)
                {
                    if (curfilefromlist.find("_1stperson\\") != NOT_FOUND) fp_animOpen = true;

                    doneAnimName = true;
                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line, match, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\"(.+?)\">")))
                    {
                        if (line.find("</hkparam>") == NOT_FOUND)
                        {
                            counter         = 0;
                            animOpen        = true;
                            elementCatch    = true;
                            string templine = line.substr(0, match.position(1));
                            openRange       = count(templine.begin(), templine.end(), '\t');
                            string number   = match[2];
                            curNum          = match[1];

                            if (!isOnlyNumber(number))
                            {
                                ErrorMessage(1174,
                                             filepath,
                                             catalyst[l].first,
                                             line.substr(line.find(
                                                 "<hkparam name=\"animationNames\" numelements=\"")));
                            }

                            base = stoi(number);
                        }
                    }
                }
                else if ((!doneDeform
                          && line.find("<hkparam name=\"deformableSkinNames\" numelements=\"") != NOT_FOUND)
                         || (!doneRigid
                             && line.find("<hkparam name=\"rigidSkinNames\" numelements=\"") != NOT_FOUND)
                         || (!doneAnimFileName
                             && line.find("<hkparam name=\"animationFilenames\" numelements=\"")
                                    != NOT_FOUND))
                {
                    line.find("<hkparam name=\"deformableSkinNames\" numelements=\"") != NOT_FOUND
                        ? doneDeform = true
                        : line.find("<hkparam name=\"rigidSkinNames\" numelements=\"") != NOT_FOUND
                              ? doneRigid        = true
                              : doneAnimFileName = true;

                    nemesis::smatch match;

                    if (nemesis::regex_search(
                            line, match, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\"(.+?)\">")))
                    {
                        if (line.find("</hkparam>") == NOT_FOUND)
                        {
                            counter         = 0;
                            otherAnimOpen   = true;
                            elementCatch    = true;
                            string templine = line.substr(0, match.position(1));
                            openRange       = count(templine.begin(), templine.end(), '\t');
                            curNum          = match[1];
                        }
                    }
                }
                else if (nemesis::regex_search(
                             line, check, nemesis::regex("<hkparam name=\"(.+?)\" numelements=\""))
                         && line.find("</hkparam>", check.position(1)) == NOT_FOUND)
                {
                    counter         = 0;
                    norElement      = true;
                    elementCatch    = true;
                    string templine = line.substr(0, check.position(1));
                    openRange       = count(templine.begin(), templine.end(), '\t');
                }
                // counting for numelement
                else if (eventOpen)
                {
                    size_t pos = line.find("</hkparam>");

                    if (pos != NOT_FOUND)
                    {
                        string templine   = line.substr(0, pos);
                        __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                        if (openRange == t_counter)
                        {
                            unordered_map<string, bool> isExist;

                            for (auto& aaEvent : AAEventName)
                            {
                                AddEvents(curNum,
                                          catalystMap[curID],
                                          aaEvent,
                                          orievent,
                                          isExist,
                                          counter,
                                          ZeroEvent,
                                          eventid,
                                          eventName);
                            }

                            SetStr codelist = BehaviorTemplate->grouplist[lowerBehaviorFile];

                            for (auto& templatecode : codelist)
                            {
                                for (auto& newEvent : newAnimEvent[templatecode + modID])
                                {
                                    AddEvents(curNum,
                                              catalystMap[curID],
                                              newEvent,
                                              orievent,
                                              isExist,
                                              counter,
                                              ZeroEvent,
                                              eventid,
                                              eventName);
                                }
                            }

                            if (eventelements == -1) eventelements = counter;

                            eventOpen                 = false;
                            if (replacedNum) {
                                replacedNum = false;
                            } else {
                                elementUpdate(elementLine, counter, curID, catalystMap);
                            }
                        }
                    }
                    else if (!replacedNum)
                    {
                        pos = line.find("<hkcstring>");

                        if (pos != NOT_FOUND)
                        {
                            pos += 11;
                            string name        = line.substr(pos, line.find("</hkcstring>", pos) - pos);
                            eventName[counter] = name;
                            eventid[name]      = counter;
                            orievent[name]     = true;

                            if (counter == 0) ZeroEvent = name;

                            ++counter;
                        }
                        else if (line.find("<hkparam name=\"flags\">") != NOT_FOUND)
                        {
                            ++counter;
                        }
                    }
                }
                else if (varOpen)
                {
                    if (!replacedNum && curNum == "wordVariableValues"
                        && line.find("<hkparam name=\"value\">") != NOT_FOUND)
                    {
                        ++counter; 
                    }
                    else if (!replacedNum && curNum == "variableInfos"
                             && line.find("<hkparam name=\"type\">") != NOT_FOUND)
                    {
                        ++counter;
                    }
                    else
                    {
                        size_t pos = line.find("</hkparam>");

                        if (pos != NOT_FOUND)
                        {
                            string templine   = line.substr(0, pos);
                            __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                            if (openRange == t_counter)
                            {
                                unordered_map<string, bool> isExist;

                                for (auto& AAVariable : AAGroupList)
                                {
                                    AddVariables(curNum,
                                                 catalystMap[curID],
                                                 "Nemesis_AA" + AAVariable,
                                                 orivariable,
                                                 isExist,
                                                 counter,
                                                 ZeroVariable,
                                                 variableid,
                                                 varName);
                                    AddVariables(curNum,
                                                 catalystMap[curID],
                                                 "Nemesis_AA_Priority" + AAVariable,
                                                 orivariable,
                                                 isExist,
                                                 counter,
                                                 ZeroVariable,
                                                 variableid,
                                                 varName);

                                    for (int k = 0; k < AAgroup_Counter[AAVariable]; ++k)
                                    {
                                        AddVariables(curNum,
                                                     catalystMap[curID],
                                                     "Nemesis_AA_Priority" + AAVariable + "_" + to_string(k),
                                                     orivariable,
                                                     isExist,
                                                     counter,
                                                     ZeroVariable,
                                                     variableid,
                                                     varName);
                                    }
                                }

                                SetStr codelist = BehaviorTemplate->grouplist[lowerBehaviorFile];

                                for (auto& templatecode : codelist)
                                {
                                    for (auto& newVariable : newAnimVariable[templatecode + modID])
                                    {
                                        AddVariables(curNum,
                                                     catalystMap[curID],
                                                     newVariable,
                                                     orivariable,
                                                     isExist,
                                                     counter,
                                                     ZeroVariable,
                                                     variableid,
                                                     varName);
                                    }
                                }

                                for (auto& modname : pceaMod)
                                {
                                    AddVariables(curNum,
                                                 catalystMap[curID],
                                                 modname,
                                                 orivariable,
                                                 isExist,
                                                 counter,
                                                 ZeroVariable,
                                                 variableid,
                                                 varName);
                                }

                                if (variableelements == -1) variableelements = counter;

                                varOpen                   = false;
                                if (replacedNum) {
                                    replacedNum = false;
                                } else {
                                    elementUpdate(elementLine, counter, curID, catalystMap);
                                }
                            }
                        }
                        else if (!replacedNum)
                        {
                            size_t pos = line.find("<hkcstring>");

                            if (curNum == "variableNames" && pos != NOT_FOUND)
                            {
                                pos += 11;
                                string name       = line.substr(pos, line.find("</hkcstring>", pos) - pos);
                                varName[counter]  = name;
                                variableid[name]  = counter;
                                orivariable[name] = true;

                                if (counter == 0) ZeroVariable = name;

                                ++counter;
                            }
                        }
                    }
                }
                else if (attriOpen)
                {
                    size_t pos = line.find("</hkparam>");

                    if (pos != NOT_FOUND)
                    {
                        string templine   = line.substr(0, pos);
                        __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                        if (openRange == t_counter)
                        {
                            if (attributeelements == -1) attributeelements = counter;

                            attriOpen = false;
                            elementUpdate(elementLine, counter, curID, catalystMap);
                        }
                    }
                    else if (line.find("<hkcstring>") != NOT_FOUND)
                    {
                        size_t nextpos = line.find("<hkcstring>") + 11;
                        string name    = line.substr(nextpos, line.find("</hkcstring>", nextpos) - nextpos);
                        attriName[counter] = name;
                        attriid[name]      = counter;
                        ++counter;
                    }
                }
                else if (charOpen)
                {
                    if (line.find("</hkparam>") != NOT_FOUND)
                    {
                        string templine   = line.substr(0, line.find("</hkparam>"));
                        __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                        if (openRange == t_counter)
                        {
                            if (characterelements == -1) characterelements = counter;

                            charOpen = false;
                            elementUpdate(elementLine, counter, curID, catalystMap);
                        }
                    }
                    else
                    {
                        size_t pos = line.find("<hkcstring>");

                        if (pos != NOT_FOUND)
                        {
                            pos += 11;
                            string name       = line.substr(pos, line.find("</hkcstring>", pos) - pos);
                            charName[counter] = name;
                            charid[name]      = counter;
                            ++counter;
                        }
                    }
                }
                else if (animOpen)
                {
                    size_t pos = line.find("</hkparam>");

                    if (pos != NOT_FOUND)
                    {
                        string templine   = line.substr(0, pos);
                        __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                        if (openRange == t_counter)
                        {
                            for (auto it = BehaviorTemplate->grouplist.begin();
                                 it != BehaviorTemplate->grouplist.end();
                                 ++it)
                            {
                                for (unsigned int k = 0; k < behaviorJoints[it->first].size(); ++k)
                                {
                                    if (lowerBehaviorFile != behaviorJoints[it->first][k]) continue;

                                    for (auto& templatecode : it->second)
                                    {
                                        if (BehaviorTemplate->optionlist[templatecode].core) continue;

                                        for (unsigned int k = 0; k < newAnimation[templatecode].size(); ++k)
                                        {
                                            if (!newAnimation[templatecode][k]->isKnown())
                                            {
                                                bool addAnim = false;
                                                string animPath
                                                    = "Animations\\"
                                                      + newAnimation[templatecode][k]->GetFilePath();
                                                AddAnims(line,
                                                         animPath,
                                                         outputdir,
                                                         behaviorFile,
                                                         lowerBehaviorFile,
                                                         newMod,
                                                         catalystMap[curID],
                                                         counter,
                                                         isAdded,
                                                         addAnim);

                                                if (addAnim) (this->*tryAddAnim)();
                                            }
                                        }
                                    }
                                }
                            }

                            if (!isFirstPerson)
                            {
                                for (auto& AA : alternateAnim)
                                {
                                    for (auto& anim : AA.second)
                                    {
                                        if (anim != "x")
                                        {
                                            bool addAnim    = false;
                                            string animPath = "Animations\\" + anim;
                                            AddAnims(line,
                                                     animPath,
                                                     outputdir,
                                                     behaviorFile,
                                                     lowerBehaviorFile,
                                                     newMod,
                                                     catalystMap[curID],
                                                     counter,
                                                     isAdded,
                                                     addAnim);

                                            if (addAnim) (this->*tryAddAnim)();
                                        }
                                    }
                                }

                                for (auto& pcea : pcealist)
                                {
                                    for (auto& animPath : pcea.animPathList)
                                    {
                                        bool addAnim = false;
                                        AddAnims(line,
                                                 animPath.second,
                                                 outputdir,
                                                 behaviorFile,
                                                 lowerBehaviorFile,
                                                 newMod,
                                                 catalystMap[curID],
                                                 counter,
                                                 isAdded,
                                                 addAnim);

                                        if (addAnim) (this->*tryAddAnim)();
                                    }
                                }
                            }

                            animOpen    = false;
                            fp_animOpen = false;
                            elementUpdate(elementLine, counter, curID, catalystMap);
                        }
                    }
                    else
                    {
                        pos = line.find("<hkcstring>");

                        if (pos != NOT_FOUND)
                        {
                            pos += 11;
                            string animPath = line.substr(pos, line.find("</hkcstring>", pos) - pos);
                            string animFile = sf::path(animPath).filename().string();

                            if (!fp_animOpen && !characterAA
                                && alternateAnim.find(nemesis::to_lower_copy(animFile))
                                       != alternateAnim.end())
                            {
                                if (!isCharacter) ErrorMessage(1184, behaviorFile);

                                characterAA = true;
                            }

                            if (activatedBehavior["gender*"])
                            {
                                if (lowerBehaviorFile == "defaultfemale")
                                {
                                    if (!nemesis::iequals(animPath, "Animations\\female\\" + animFile))
                                    {
                                        sf::path animation(GetFileDirectory(outputdir));

                                        if (isFileExist(animation.parent_path().parent_path().string()
                                                        + "\\Animations\\female\\" + animFile))
                                        {
                                            line.replace(
                                                pos, animPath.length(), "Animations\\female\\" + animFile);
                                        }
                                        else if (nemesis::iequals(animPath, "Animations\\male\\" + animFile))
                                        {
                                            if (isFileExist(animation.parent_path().parent_path().string()
                                                            + "\\Animations\\" + animFile))
                                            {
                                                line.replace(
                                                    pos, animPath.length(), "Animations\\" + animFile);
                                            }
                                        }
                                    }
                                }
                                else if (lowerBehaviorFile == "defaultmale"
                                         && !nemesis::iequals(animPath, "Animations\\male\\" + animFile))
                                {
                                    sf::path animation(GetFileDirectory(outputdir));

                                    if (isFileExist(animation.parent_path().parent_path().string()
                                                    + "\\Animations\\male\\" + animFile))
                                    {
                                        line.replace(pos, animPath.length(), "Animations\\male\\" + animFile);
                                    }
                                    else if (nemesis::iequals(animPath, "Animations\\female\\" + animFile))
                                    {
                                        if (isFileExist(animation.parent_path().parent_path().string()
                                                        + "\\Animations\\" + animFile))
                                        {
                                            line.replace(pos, animPath.length(), "Animations\\" + animFile); 
                                        }
                                    }
                                }
                            }

                            nemesis::to_lower(animPath);
                            nemesis::to_lower(animFile);
                            isAdded[animPath] = true;
                            registeredAnim[lowerBehaviorFile].insert(animFile);

                            Lockless locker(animdata_lock);
                            shared_ptr<AnimationDataTracker>& animData
                                = charAnimDataInfo[lowerBehaviorFile][animFile];

                            if (animData == nullptr)
                                animData = make_shared<AnimationDataTracker>(counter, animFile);
                            else
                                animData->SetOrder(counter);

                            locker.Unlock();
                            (this->*tryAddAnim)();
                            ++counter;

                            if (newMod.length() == 0) newMod = "Skyrim";

                            vector<set<string>>* match_ptr = &animModMatch[lowerBehaviorFile][animFile];
                            size_t matchSize               = match_ptr->size();

                            if (matchSize == 0)
                            {
                                match_ptr->push_back(set<string>{animPath});
                                match_ptr->push_back(set<string>{newMod});
                            }
                            else if (matchSize == 2)
                            {
                                match_ptr->at(0).insert(animPath);
                                match_ptr->at(1).insert(newMod);
                            }
                            else
                            {
                                ErrorMessage(1058);
                            }
                        }
                    }
                }
                else if (otherAnimOpen)
                {
                    size_t pos = line.find("</hkparam>");

                    if (pos != NOT_FOUND)
                    {
                        string templine   = line.substr(0, pos);
                        __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                        if (openRange == t_counter)
                        {
                            otherAnimOpen = false;
                            elementUpdate(elementLine, counter, curID, catalystMap);
                        }
                    }
                    else if (line.find("<hkcstring>") != NOT_FOUND)
                    {
                        ++counter;
                    }
                }
                else if (norElement)
                {
                    size_t pos = line.find("</hkparam>");

                    if (pos != NOT_FOUND)
                    {
                        string templine   = line.substr(0, pos);
                        __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                        if (openRange == t_counter)
                        {
                            norElement = false;
                            elementUpdate(elementLine, counter, curID, catalystMap);
                        }
                    }
                    else
                    {
                        pos = line.find("<hkobject>");

                        if (pos != NOT_FOUND)
                        {
                            string templine   = line.substr(0, pos);
                            __int64 t_counter = count(templine.begin(), templine.end(), '\t');

                            if (t_counter == openRange + 1) ++counter;
                        }
                        else if (line.find("\t\t\t#") != NOT_FOUND)
                        {
                            string templine  = line.substr(0, line.find("#"));
                            size_t reference = count(templine.begin(), templine.end(), '\t');

                            if (reference == openRange + 1)
                            {
                                int number = int(count(line.begin(), line.end(), '#'));
                                counter += number;
                            }
                        }
                    }
                }

                if (error) throw nemesis::exception();

                int repeatnum = 1;

                if (line.find("$") != NOT_FOUND)
                {
                    size_t pos = line.find("<!-- Bone$N -->");

                    if (pos != NOT_FOUND && pos == line.length() - 15)
                    {
                        if (newBone)
                            repeatnum = bonenum;
                        else
                            repeatnum = 0;
                    }

                    if (!ignoreFunction[curfilefromlist][curID])
                    {
                        if (line.find("$eventID[", 0) != NOT_FOUND && line.find("]$", 0) != NOT_FOUND)
                        {
                            string change = nemesis::regex_replace(
                                string(line), nemesis::regex(".*[$](eventID[[].*[]])[$].*"), string("\\1"));

                            if (change != line)
                            {
                                string oldChange = "$" + change + "$";
                                eventIDReplacer(change, "BASE", behaviorFile, eventid, ZeroEvent, catalyst[l].first);
                                line.replace(line.find(oldChange), oldChange.length(), change);
                            }

                            if (error) throw nemesis::exception();
                        }

                        if (line.find("$variableID[", 0) != NOT_FOUND)
                        {
                            string change
                                = nemesis::regex_replace(string(line),
                                                         nemesis::regex(".*[$](variableID[[].*[]])[$].*"),
                                                         string("\\1"));

                            if (change != line)
                            {
                                string oldChange = "$" + change + "$";
                                variableIDReplacer(change,
                                                   "BASE",
                                                   behaviorFile,
                                                   variableid,
                                                   ZeroVariable,
                                                   catalyst[l].first);
                                line.replace(line.find(oldChange), oldChange.length(), change);
                            }

                            if (error) throw nemesis::exception();
                        }
                    }

                    if (line.find("$stateID[", 0) != NOT_FOUND)
                    {
                        string change = nemesis::regex_replace(
                            string(line),
                            nemesis::regex(".*[$](stateID[[].*[]][[][0-9]+[]][[].*[]][[][0-9]+[]])[$].*"),
                            string("\\1"));

                        if (change != line)
                        {
                            string oldChange = "$" + change + "$";

                            if (registeredStateID[change].length() > 0)
                            {
                                line.replace(
                                    line.find(oldChange), oldChange.length(), registeredStateID[change]);
                            }
                            else
                            {
                                string node = nemesis::regex_replace(
                                    string(line),
                                    nemesis::regex(
                                        ".*[$]stateID[[](.*)[]][[]([0-9]+)[]][[].*[]][[][0-9]+[]][$].*"),
                                    string("\\1"));
                                string base = nemesis::regex_replace(
                                    string(line),
                                    nemesis::regex(
                                        ".*[$]stateID[[](.*)[]][[]([0-9]+)[]][[].*[]][[][0-9]+[]][$].*"),
                                    string("\\2"));

                                if (stateStrID[node + base] == 0)
                                    stateStrID[node + base] = stoi(base) + 1;
                                else
                                    base = to_string(stateStrID[node + base]++);

                                line.replace(line.find(oldChange), oldChange.length(), base);
                                registeredStateID[change] = base;
                            }
                        }
                        else
                        {
                            ErrorMessage(1186, filepath, catalyst[l].first, change);
                        }
                    }
                }

                if (isClipTrigger && line.find("<hkparam name=\"localTime\">-") != NOT_FOUND)
                {
                    negative = true; 
                }
                else if (negative && line.find("<hkparam name=\"relativeToEndOfClip\">") != NOT_FOUND)
                {
                    if (line.find("<hkparam name=\"relativeToEndOfClip\">true</hkparam>") == NOT_FOUND)
                    {
                        size_t pos = line.find("<hkparam name=\"relativeToEndOfClip\">") + 36;
                        line.replace(pos, line.find("</hkparam>", pos) - pos, "true");
                    }

                    negative = false;
                }

                int repeated = 0;

                while (repeated < repeatnum)
                {
                    string newline = line;

                    if (newBone && newline.find("%n") != NOT_FOUND)
                    {
                        while (newline.find("%n") != NOT_FOUND)
                        {
                            newline.replace(newline.find("%n"), 2, to_string(repeated + oribone));
                        }
                    }

                    catalystMap[curID].push_back(newline);
                    ++repeated;
                }

                if (elementCatch) elementLine = catalystMap[curID].size() - 1;
            }

            if (error) throw nemesis::exception();
        }

        DebugLogging(L"Processing behavior: " + filepath
                     + L" (Check point 3, Behavior general processing complete)");

        if (clipAA.size() == 0 && pceaID.size() == 0 && !activatedBehavior[lowerBehaviorFile] && !characterAA
            && !newBone)
        {
            if (!isEdited(BehaviorTemplate.get(), lowerBehaviorFile, newAnimation, isCharacter, modID))
            {
                // final output
#ifdef DEBUG
                outputdir = "new_behaviors\\"
                            + behaviorPath[lowerBehaviorFile].substr(
                                behaviorPath[lowerBehaviorFile].find("\\") + 1);
#else
                outputdir = behaviorPath[nemesis::transform_to<wstring>(lowerBehaviorFile)] + L".hkx";
#endif
                if (SSE) lowerBehaviorFile = "SSE\\" + lowerBehaviorFile;

                string cachedFile = "cached_behaviors\\" + lowerBehaviorFile + ".hkx";

                redirToStageDir(outputdir, nemesisInfo);

                if (!FolderCreate(GetFileDirectory(outputdir))) return;

                if (isFileExist(cachedFile) && !sf::is_directory(cachedFile))
                {
                    sf::copy_file(cachedFile, outputdir, sf::copy_options::overwrite_existing); 
                }
                else if (isFileExist(outputdir) && !sf::is_directory(outputdir))
                {
                    if (!sf::remove(outputdir)) WarningMessage(1005, outputdir);
                }

                int i = 0;

                while (i < 8)
                {
                    process->newMilestone();
                    ++i;
                }

                DebugLogging(L"Processing behavior: " + filepath + L" (Check point 3.4, No changes detected)");
                return;
            }
        }
    }

    if (catalystMap[curID].size() != 0)
    {
        if (catalystMap[curID].back().empty())
        {
            if (catalystMap[curID].size() > 1 && catalystMap[curID][catalystMap[curID].size() - 2].empty())
                catalystMap[curID].pop_back();
        }
        else
        {
            catalystMap[curID].push_back("");
        }
    }

    catalystMap[curID].shrink_to_fit();
    process->newMilestone();

    // newAnimation ID in existing function
    if (catcher.size() != 0)
    {
        for (auto it = catcher.begin(); it != catcher.end(); ++it)
        {
            for (unsigned int k = 0; k < it->second.size(); ++k)
            {
                int tempID   = it->second[k].getID();
                int tempLine = it->second[k].getLine();
                string* line = &catalystMap[it->second[k].getID()][it->second[k].getLine()];

                if (IDExist[it->first].length() != 0)
                {
                    line->append("%");
                    string masterFormat = it->first.substr(0, it->first.find("$"));
                    string numID        = it->first.substr(it->first.find("$") + 1);
                    nemesis::smatch match;
                    nemesis::regex_match(
                        *line, match, nemesis::regex(".*(" + masterFormat + "[$]" + numID + ")[^0-9]+.*"));
                    line->replace(match.position(1), it->first.length(), IDExist[it->first]);
                    line->pop_back();
                }
                else
                {
                    ErrorMessage(1026, filepath, it->first.substr(0, it->first.find("$")));
                }
            }
        }
    }

    if (error) throw nemesis::exception();

    process->newMilestone();

    // add new animation
    vector<shared_ptr<VecStr>> allEditLines;
    unordered_map<string, bool> isCoreDone;
    unordered_map<int, int> functionState;
    unordered_map<int, shared_ptr<NodeJoint>> existingNodes;
    int grouptimer = 0;
    int onetimer   = 0;

    // check for having newAnimation for the file
    auto tempGp = BehaviorTemplate->grouplist.find(lowerBehaviorFile);

    if (tempGp != BehaviorTemplate->grouplist.end())
    {
        for (auto& templateCode : tempGp->second)
        {
            VecStr opening;
            opening.push_back("<!-- ======================== NEMESIS " + templateCode
                              + " TEMPLATE START ======================== -->");
            allEditLines.emplace_back(make_shared<VecStr>(opening));
            bool hasGroup    = false;
            bool hasMaster   = false;
            bool ignoreGroup = false;

            auto bhvtmpcode = BehaviorTemplate->behaviortemplate.find(templateCode + "_group");

            if (bhvtmpcode != BehaviorTemplate->behaviortemplate.end())
            {
                auto codefile = bhvtmpcode->second.find(lowerBehaviorFile);

                if (codefile != bhvtmpcode->second.end())
                {
                    if (codefile->second.size() > 0) hasGroup = true;
                }
            }

            auto btitr = BehaviorTemplate->behaviortemplate.find(templateCode + "_master");

            if (btitr != BehaviorTemplate->behaviortemplate.end())
            {
                auto btlbitr = btitr->second.find(lowerBehaviorFile);

                if (btlbitr != btitr->second.end())
                {
                    if (btlbitr->second.size() > 0) hasMaster = true;
                }
            }

            Lockless nalock(process->getNewAnimFlag());
            auto aitr = newAnimation.find(templateCode);

            if (aitr != newAnimation.end() && aitr->second.size() != 0)
            {
                auto newAnimCopy = aitr->second;
                nalock.Unlock();

                shared_ptr<master> groupFunctionIDs = make_shared<master>();
                vector<vector<shared_ptr<AnimationInfo>>> groupAnimInfo;

                allEditLines.emplace_back(make_shared<VecStr>(VecStr{""}));

                if (!newAnimSkip(newAnimCopy, modID))
                {
                    shared_ptr<group> subFunctionIDs = make_shared<group>();
                    subFunctionIDs->singlelist.reserve(memory);
                    group newSubFunctionIDs;
                    vector<int> stateID;
                    vector<int> stateMultiplier = {0};
                    GetStateCount(stateMultiplier,
                                  BehaviorTemplate->behaviortemplate[templateCode][lowerBehaviorFile],
                                  templateCode,
                                  behaviorFile,
                                  hasGroup);

                    if (!hasGroup)
                    {
                        stateID
                            = GetStateID(BehaviorTemplate->mainBehaviorJoint[templateCode][lowerBehaviorFile],
                                         catalystMap,
                                         functionState);

                        if (newAnimCopy[0]->GetGroupAnimInfo()[0]->ignoreGroup)
                        {
                            ignoreGroup = true;
                            // newSubFunctionIDs.singlelist.reserve(memory);
                            groupFunctionIDs->grouplist.reserve(memory);
                        }
                    }
                    else
                    {
                        stateID.push_back(0);
                        groupFunctionIDs->grouplist.reserve(memory);
                    }

                    if (error) throw nemesis::exception();

                    if (stateMultiplier.size() != stateID.size() || stateMultiplier.size() == 0)
                    {
                        ErrorMessage(1192);
                    }

                    size_t newAnimCount = newAnimCopy.size();

                    if (newAnimCount > 0)
                    {
                        DebugLogging(L"Processing behavior: " + filepath + L" (Check point 3.6, Mod code: "
                                     + nemesis::transform_to<wstring>(templateCode) + L", Animation count: "
                                     + to_wstring(newAnimCount) + L")");
                        shared_ptr<NewAnimation> dummyAnimation;
                        //int IDMultiplier = newAnimCopy[0]->getNextID(lowerBehaviorFile);
                        NewAnimLock animLock;
                        nemesis::ThreadPool tp;
                        auto start_time = chrono::steady_clock::now();

                        // individual animation
                        if (hasGroup)
                        {
                            for (unsigned int k = 0; k < newAnimCount; ++k)
                            {
                                try
                                {
                                    if (newAnimCopy[k]->coreModID != modID)
                                    {
                                        if (dummyAnimation != nullptr && !dummyAnimation->isLast())
                                        {
                                            ErrorMessage(1183);
                                        }
                                    }
                                    else
                                    {
                                        subFunctionIDs->singlelist.emplace_back(make_shared<single>());
                                        subFunctionIDs->singlelist.back()
                                            ->format["Nemesis" + modID + lowerBehaviorFile + to_string(k)]
                                            = to_string(k);
                                        allEditLines.emplace_back(make_shared<VecStr>());
                                        dummyAnimation = newAnimCopy[k];

                                        shared_ptr<NewAnimArgs> args = make_shared<NewAnimArgs>(
                                            modID,
                                            lowerBehaviorFile,
                                            &lastID,
                                            BehaviorTemplate->optionlist[templateCode].core,
                                            newAnimCopy[k],
                                            allEditLines.back(),
                                            isCoreDone[newAnimCopy[k]
                                                           ->GetGroupAnimInfo()[0]
                                                           ->filename],
                                            functionState,
                                            exportID,
                                            eventid,
                                            variableid,
                                            animLock,
                                            ZeroEvent,
                                            ZeroVariable,
                                            hasGroup,
                                            stateID,
                                            stateMultiplier,
                                            subFunctionIDs->singlelist.back(),
                                            subFunctionIDs,
                                            groupFunctionIDs,
                                            false,
                                            groupAnimInfo);

                                        if (newAnimCopy[k]->isLast())
                                        {
                                            subFunctionIDs->singlelist.shrink_to_fit();
                                            groupFunctionIDs->grouplist.push_back(subFunctionIDs);
                                            subFunctionIDs = make_shared<group>();
                                            subFunctionIDs->singlelist.reserve(memory);
                                            groupAnimInfo.push_back(newAnimCopy[k]->GetGroupAnimInfo());

                                            for (unsigned int statenum = 0; statenum < stateID.size();
                                                 ++statenum)
                                            {
                                                stateID[statenum] = 0;
                                            }
                                        }
                                        else
                                        {
                                            for (unsigned int statenum = 0; statenum < stateID.size();
                                                 ++statenum)
                                            {
                                                stateID[statenum] += stateMultiplier[statenum];
                                            }
                                        }

                                        if (error) throw nemesis::exception();

                                        //lastID += IDMultiplier;
                                        tp.enqueue(animThreadStart, args);
                                        //animThreadStart(args);
                                    }
                                }
                                catch (nemesis::exception&)
                                {
                                    // resolved exception
                                }
                            }
                        }
                        else
                        {
                            for (unsigned int k = 0; k < newAnimCount; ++k)
                            {
                                try
                                {
                                    if (newAnimCopy[k]->coreModID != modID)
                                    {
                                        if (dummyAnimation != nullptr && !dummyAnimation->isLast())
                                        {
                                            ErrorMessage(1183);
                                        }
                                    }
                                    else
                                    {
                                        subFunctionIDs->singlelist.emplace_back(make_shared<single>());
                                        subFunctionIDs->singlelist.back()
                                            ->format["Nemesis" + modID + lowerBehaviorFile + to_string(k)]
                                            = to_string(k);
                                        allEditLines.emplace_back(make_shared<VecStr>());
                                        dummyAnimation = newAnimCopy[k];

                                        shared_ptr<NewAnimArgs> args = make_shared<NewAnimArgs>(
                                            modID,
                                            lowerBehaviorFile,
                                            &lastID,
                                            BehaviorTemplate->optionlist[templateCode].core,
                                            newAnimCopy[k],
                                            allEditLines.back(),
                                            isCoreDone[newAnimCopy[k]->GetGroupAnimInfo()[0]->filename],
                                            functionState,
                                            exportID,
                                            eventid,
                                            variableid,
                                            animLock,
                                            ZeroEvent,
                                            ZeroVariable,
                                            hasGroup,
                                            stateID,
                                            stateMultiplier,
                                            subFunctionIDs->singlelist.back(),
                                            subFunctionIDs,
                                            groupFunctionIDs,
                                            ignoreGroup,
                                            groupAnimInfo);

                                        if (ignoreGroup)
                                        {
                                            if (newAnimCopy[k]->isLast())
                                            {
                                                subFunctionIDs->singlelist.shrink_to_fit();
                                                groupFunctionIDs->grouplist.push_back(subFunctionIDs);
                                                subFunctionIDs = make_shared<group>();
                                                subFunctionIDs->singlelist.reserve(memory);
                                                groupAnimInfo.push_back(newAnimCopy[k]->GetGroupAnimInfo());

                                                for (unsigned int statenum = 0; statenum < stateID.size();
                                                     ++statenum)
                                                {
                                                    stateID[statenum] += stateMultiplier[statenum];
                                                }
                                            }
                                        }
                                        else
                                        {
                                            groupAnimInfo.push_back(newAnimCopy[k]->GetGroupAnimInfo());

                                            for (unsigned int statenum = 0; statenum < stateID.size();
                                                 ++statenum)
                                            {
                                                stateID[statenum] += stateMultiplier[statenum];
                                            }
                                        }

                                        if (error) throw nemesis::exception();

                                        //lastID += IDMultiplier;
                                        tp.enqueue(animThreadStart, args);
                                        //animThreadStart(args);
                                    }
                                }
                                catch (nemesis::exception&)
                                {
                                    // resolved exception
                                }
                            }
                        }

                        tp.join_all();

                        if (!ignoreGroup && !hasGroup)
                        {
                            subFunctionIDs->singlelist.shrink_to_fit();
                            groupFunctionIDs->grouplist.push_back(subFunctionIDs);
                            subFunctionIDs = make_shared<group>();
                            vector<shared_ptr<AnimationInfo>> subGroupAnimInfo;
                            vector<vector<shared_ptr<AnimationInfo>>> newGroupAnimInfo;

                            for (unsigned int l = 0; l < groupAnimInfo.size(); ++l)
                            {
                                subGroupAnimInfo.push_back(groupAnimInfo[l][0]);
                            }

                            newGroupAnimInfo.push_back(subGroupAnimInfo);
                            groupAnimInfo = newGroupAnimInfo;
                        }

                        // check error before initialization
                        for (unsigned int curGroup = 0; curGroup < groupFunctionIDs->grouplist.size();
                             ++curGroup)
                        {
                            if (groupFunctionIDs->grouplist[curGroup]->singlelist.size()
                                != groupAnimInfo[curGroup].size())
                            {
                                ErrorMessage(1156, templateCode);
                            }
                        }

                        auto diff = chrono::steady_clock::now() - start_time;
                        onetimer += chrono::duration_cast<chrono::milliseconds>(diff).count();
                        start_time = chrono::steady_clock::now();

                        // group animation
                        if (hasGroup && groupFunctionIDs->grouplist.size() > 0)
                        {
                            // Preload Template to memory
                            string filename                             = templateCode + "_group";
                            shared_ptr<AnimTemplate> grouptemplate_pack = make_shared<AnimTemplate>();
                            grouptemplate_pack->ExamineTemplate(
                                templateCode,
                                lowerBehaviorFile,
                                BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile],
                                true,
                                false,
                                BehaviorTemplate->optionlist[templateCode]);
                            //IDMultiplier = getTemplateNextID(
                            //    BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile]);
                            stateMultiplier = vector<int>();

                            if (!GetStateCount(
                                    stateMultiplier,
                                    BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile],
                                    templateCode,
                                    behaviorFile,
                                    hasGroup))
                                return;

                            hasMaster
                                ? stateID = {0}
                                : stateID = GetStateID(
                                      BehaviorTemplate->mainBehaviorJoint[templateCode][lowerBehaviorFile],
                                      catalystMap,
                                      functionState);
                            size_t n_newAnimCount = groupFunctionIDs->grouplist.size();

                            // Execute group template from memory
                            nemesis::ThreadPool tp2;

                            for (unsigned int k = 0; k < n_newAnimCount; ++k)
                            {
                                try
                                {
                                    allEditLines.emplace_back(make_shared<VecStr>());
                                    shared_ptr<GroupTemplate> groupTemp = make_shared<GroupTemplate>(
                                        BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile],
                                        grouptemplate_pack);
                                    groupTemp->setZeroEvent(ZeroEvent);
                                    groupTemp->setZeroVariable(ZeroVariable);
                                    shared_ptr<newGroupArgs> args
                                        = make_shared<newGroupArgs>(templateCode,
                                                                    lowerBehaviorFile,
                                                                    filename,
                                                                    &lastID,
                                                                    k + 1,
                                                                    stateID,
                                                                    groupTemp,
                                                                    allEditLines.back(),
                                                                    exportID,
                                                                    eventid,
                                                                    variableid,
                                                                    animLock,
                                                                    ZeroEvent,
                                                                    ZeroVariable,
                                                                    groupFunctionIDs,
                                                                    groupAnimInfo);

                                    for (unsigned int statenum = 0; statenum < stateID.size(); ++statenum)
                                    {
                                        stateID[statenum] += stateMultiplier[statenum];
                                    }

                                    if (error) throw nemesis::exception();

                                    //lastID += IDMultiplier;
                                    tp2.enqueue(groupThreadStart, args);
                                }
                                catch (nemesis::exception&)
                                {
                                    // resolved exception
                                }
                            }

                            tp2.join_all();
                        }

                        // master animation
                        if (hasMaster)
                        {
                            // Preload master template to memory
                            string filename                              = templateCode + "_master";
                            shared_ptr<AnimTemplate> mastertemplate_pack = make_shared<AnimTemplate>();
                            mastertemplate_pack->ExamineTemplate(
                                templateCode,
                                lowerBehaviorFile,
                                BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile],
                                true,
                                true,
                                BehaviorTemplate->optionlist[templateCode]);
                            stateID = GetStateID(
                                BehaviorTemplate->mainBehaviorJoint[templateCode][lowerBehaviorFile],
                                catalystMap,
                                functionState);

                            // Execute master template
                            shared_ptr<GroupTemplate> masterTemp = make_shared<GroupTemplate>(
                                BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile],
                                mastertemplate_pack);
                            masterTemp->setZeroEvent(ZeroEvent);
                            masterTemp->setZeroVariable(ZeroVariable);
                            allEditLines.emplace_back(make_shared<VecStr>());
                            masterTemp->getFunctionLines(allEditLines.back(),
                                                         lowerBehaviorFile,
                                                         filename,
                                                         stateID,
                                                         groupFunctionIDs,
                                                         groupAnimInfo,
                                                         &lastID,
                                                         exportID,
                                                         eventid,
                                                         variableid,
                                                         templateCode,
                                                         animLock,
                                                         -1);

                            //lastID += getTemplateNextID(
                            //    BehaviorTemplate->behaviortemplate[filename][lowerBehaviorFile]);

                            if (error) throw nemesis::exception();
                        }

                        diff = chrono::steady_clock::now() - start_time;
                        grouptimer += chrono::duration_cast<chrono::milliseconds>(diff).count();
                        DebugLogging(L"Processing behavior: " + filepath
                                     + L" (Check point 3.8, Mod code: " + nemesis::transform_to<wstring>(templateCode)
                                     + L", Animation count: " + to_wstring(newAnimCount) + L" COMPLETE)");
                    }
                }

                DebugLogging(
                    L"Processing behavior: " + filepath + L" (Check point 3.8, Mod code: "
                    + nemesis::transform_to<wstring>(templateCode) + L", Existing ID count: "
                    + to_wstring(BehaviorTemplate->existingFunctionID[templateCode][lowerBehaviorFile].size())
                    + L")");

                processExistFuncID(BehaviorTemplate->existingFunctionID[templateCode][lowerBehaviorFile],
                                   ZeroEvent,
                                   ZeroVariable,
                                   catalystMap,
                                   groupFunctionIDs,
                                   groupAnimInfo,
                                   templateCode,
                                   exportID,
                                   eventid,
                                   variableid,
                                   lastID,
                                   hasMaster,
                                   hasGroup,
                                   BehaviorTemplate->grouplist[lowerBehaviorFile],
                                   ignoreGroup,
                                   behaviorFile,
                                   existingNodes);

                DebugLogging(
                    L"Processing behavior: " + filepath + L" (Check point 3.8, Mod code: "
                    + nemesis::transform_to<wstring>(templateCode) + L", Existing ID count: "
                    + to_wstring(BehaviorTemplate->existingFunctionID[templateCode][lowerBehaviorFile].size())
                    + L" COMPLETE)");
            }
            else
            {
                nalock.Unlock();

                DebugLogging(
                    L"Processing behavior: " + filepath + L" (Check point 3.8, Mod code: "
                    + nemesis::transform_to<wstring>(templateCode) + L", Existing ID count: "
                    + to_wstring(BehaviorTemplate->existingFunctionID[templateCode][lowerBehaviorFile].size())
                    + L")");

                processExistFuncID(BehaviorTemplate->existingFunctionID[templateCode][lowerBehaviorFile],
                                   ZeroEvent,
                                   ZeroVariable,
                                   catalystMap,
                                   shared_ptr<master>(),
                                   vector<vector<shared_ptr<AnimationInfo>>>(),
                                   templateCode,
                                   exportID,
                                   eventid,
                                   variableid,
                                   lastID,
                                   hasMaster,
                                   hasGroup,
                                   BehaviorTemplate->grouplist[lowerBehaviorFile],
                                   ignoreGroup,
                                   behaviorFile,
                                   existingNodes);

                DebugLogging(
                    L"Processing behavior: " + filepath + L" (Check point 3.8, Mod code: "
                    + nemesis::transform_to<wstring>(templateCode) + L", Existing ID count: "
                    + to_wstring(BehaviorTemplate->existingFunctionID[templateCode][lowerBehaviorFile].size())
                    + L" COMPLETE)");
            }

            VecStr closing;
            closing.push_back("<!-- ======================== NEMESIS " + templateCode
                              + " TEMPLATE END ======================== -->");
            closing.push_back("");
            allEditLines.emplace_back(make_unique<VecStr>(closing));
        }
    }

    // check for error
    if (error) throw nemesis::exception();

    // load to existing Nodes from behavior template
    unpackToCatalyst(catalystMap, existingNodes);

    DebugLogging("Total single animation processing time for " + behaviorFile + ": " + to_string(onetimer));
    DebugLogging("Total group animation processing time for " + behaviorFile + ": " + to_string(grouptimer));
    DebugLogging(L"Processing behavior: " + filepath + L" (Check point 4, New animation inclusion complete)");

    process->newMilestone();

    // AA animation installation
    VecStr AAlines;

    if (clipAA.size() != 0)
    {
        DebugLogging(L"Processing behavior: " + filepath
                     + L" (Check point 4.2, AA count: " + to_wstring(clipAA.size()) + L")");
        unordered_map<string, int> replacerCount;

        for (auto iter = clipAA.begin(); iter != clipAA.end(); ++iter)
        {
            bool isChange = false;
            VecStr msglines;

            if (error) throw nemesis::exception();

            for (auto it = iter->second.begin(); it != iter->second.end(); ++it)
            {
                for (auto& animName : it->second)
                {
                    if (animName != "x")
                    {
                        isChange = true;
                        break;
                    }
                }

                if (!isChange) break;

                VecStr children;
                string groupName  = AAGroup[it->first];
                string importline = to_string(iter->first);

                while (importline.length() < 4)
                {
                    importline = "0" + importline;
                }

                msglines.push_back("		<hkobject name=\"#" + importline
                                   + "\" class=\"hkbManualSelectorGenerator\" signature=\"0xd932fab8\">");
                importline = "variableID[Nemesis_AA" + (isFirstPerson ? groupName + "_1p*" : groupName) + "]";
                variableIDReplacer(importline, "AA", behaviorFile, variableid, ZeroVariable, 0);

                if (error) throw nemesis::exception();

                if (exportID["variable_binding"]["selectedGeneratorIndex!~^!" + importline].length() == 0)
                {
                    string tempID = to_string(lastID);

                    while (tempID.length() < 4)
                    {
                        tempID = "0" + tempID;
                    }

                    exportID["variable_binding"]["selectedGeneratorIndex!~^!" + importline] = tempID;
                    importline = "			<hkparam name=\"variableBindingSet\">#" + tempID + "</hkparam>";
                    ++lastID;

                    if (lastID == 9216) ++lastID;
                }
                else
                {
                    importline = "			<hkparam name=\"variableBindingSet\">#"
                                 + exportID["variable_binding"]["selectedGeneratorIndex!~^!" + importline]
                                 + "</hkparam>";
                }

                msglines.push_back(importline);
                msglines.push_back("			<hkparam name=\"userData\">0</hkparam>");
                replacerCount[it->first]++;
                msglines.push_back("			<hkparam name=\"name\">Nemesis_"
                                   + to_string(replacerCount[it->first]) + "_" + groupName
                                   + "_MSG</hkparam>");
                msglines.push_back("			<hkparam name=\"generators\" numelements=\""
                                   + to_string(it->second.size() + 1) + "\">");

                string baseID = to_string(lastID);

                while (baseID.length() < 4)
                {
                    baseID = "0" + baseID;
                }

                msglines.push_back("				#" + baseID);
                ++lastID;

                if (lastID == 9216) ++lastID;

                for (unsigned int i = 0; i < it->second.size(); ++i)
                {
                    if (it->second[i] != "x")
                    {
                        string tempID = to_string(lastID);

                        while (tempID.length() < 4)
                        {
                            tempID = "0" + tempID;
                        }

                        msglines.push_back("				#" + tempID);
                        children.push_back(tempID);
                        ++lastID;

                        if (lastID == 9216) ++lastID;
                    }
                    else
                    {
                        msglines.push_back("				#" + baseID);
                    }
                }

                msglines.push_back("			</hkparam>");
                msglines.push_back("			<hkparam name=\"selectedGeneratorIndex\">0</hkparam>");
                msglines.push_back("			<hkparam name=\"currentGeneratorIndex\">0</hkparam>");
                msglines.push_back("		</hkobject>");
                msglines.push_back("");

                unordered_map<string, VecStr> triggerID;
                string name;
                string animpath;

                {
                    int i_baseID       = stoi(baseID);
                    auto pceaBaseIter = pceaID.find(iter->first);
                    catalystMap[i_baseID].reserve(catalystMap[iter->first].size());
                    catalystMap[i_baseID].push_back(
                        "		<hkobject name=\"#" + baseID
                        + "\" class=\"hkbClipGenerator\" signature=\"0x333b85b9\">");

                    for (unsigned int i = 1; i < catalystMap[iter->first].size(); ++i)
                    {
                        string line = catalystMap[iter->first][i];
                        catalystMap[i_baseID].push_back(line);
                        size_t pos = line.find("<hkparam name=\"name\">");

                        if (pos != NOT_FOUND)
                        {
                            pos += 21;
                            name = line.substr(pos, line.find("</hkparam>", pos) - pos);
                        }
                        else
                        {
                            pos = line.find("<hkparam name=\"animationName\">");

                            if (pos != NOT_FOUND)
                            {
                                pos += 30;
                                animpath = line.substr(pos, line.find("</hkparam>", pos) - pos);
                            }
                        }
                    }

                    if (catalystMap[i_baseID].back().length() != 0) catalystMap[i_baseID].push_back("");

                    if (pceaBaseIter != pceaID.end())
                    {
                        pceaID[i_baseID] = pceaBaseIter->second;
                        pceaID.erase(pceaBaseIter);
                    }
                }

                int num = 0;
                AAlines.reserve(catalystMap[iter->first].size() * children.size());

                for (unsigned int i = 0; i < children.size(); ++i)
                {
                    AAlines.push_back("		<hkobject name=\"#" + children[i]
                                      + "\" class=\"hkbClipGenerator\" signature=\"0x333b85b9\">");
                    AAlines.push_back(catalystMap[iter->first][1]);
                    AAlines.push_back(catalystMap[iter->first][2]);
                    AAlines.push_back(catalystMap[iter->first][3]);

                    while (it->second[num] == "x")
                    {
                        ++num;
                    }

                    AAlines.push_back("			<hkparam name=\"animationName\">Animations\\"
                                      + it->second[num] + "</hkparam>");
                    string animFile = nemesis::to_lower_copy(
                        it->second[num].substr(it->second[num].find_last_of("\\") + 1));
                    auto aaEvent_itr = AAEvent.find(isFirstPerson ? animFile + "_1p*" : animFile);

                    if (aaEvent_itr != AAEvent.end())
                    {
                        if (aaEvent_itr->second.size() > 0)
                        {
                            string tempID = to_string(lastID);

                            while (tempID.length() < 4)
                            {
                                tempID = "0" + tempID;
                            }

                            AAlines.push_back("			<hkparam name=\"triggers\">#" + tempID
                                              + "</hkparam>");
                            triggerID[tempID] = aaEvent_itr->second;
                            ++lastID;

                            if (lastID == 9216) ++lastID;
                        }
                        else
                        {
                            AAlines.push_back("			<hkparam name=\"triggers\">null</hkparam>");
                        }
                    }
                    else
                    {
                        AAlines.push_back(catalystMap[iter->first][5]);
                    }

                    for (unsigned int j = 6; j < catalystMap[iter->first].size(); ++j)
                    {
                        AAlines.push_back(catalystMap[iter->first][j]);
                    }

                    if (AAlines.back().length() != 0) AAlines.push_back("");

                    if (error) throw nemesis::exception();

                    ++num;
                }

                for (auto itera = triggerID.begin(); itera != triggerID.end(); ++itera)
                {
                    AAlines.reserve(AAlines.size() + (itera->second.size() * 12) + 5);
                    AAlines.push_back("		<hkobject name=\"#" + itera->first
                                      + "\" class=\"hkbClipTriggerArray\" signature=\"0x59c23a0f\">");
                    AAlines.push_back("			<hkparam name=\"triggers\" numelements=\""
                                      + to_string(itera->second.size() / 2) + "\">");

                    for (unsigned int i = 0; i < itera->second.size(); ++i)
                    {
                        bool negative = false;
                        string time   = itera->second[i + 1];

                        if (time[0] == '-')
                        {
                            negative        = true;
                            string signTime = time.substr(1);

                            if (isOnlyNumber(signTime) && stod(signTime) == 0) time = signTime;
                        }

                        AAlines.push_back("				<hkobject>");
                        AAlines.push_back("					<hkparam name=\"localTime\">" + time
                                          + "</hkparam>");
                        AAlines.push_back("					<hkparam name=\"event\">");
                        AAlines.push_back("						<hkobject>");

                        string eventID = "eventID[" + itera->second[i] + "]";
                        eventIDReplacer(eventID, "AA", behaviorFile, eventid, ZeroEvent, 0);

                        if (error) throw nemesis::exception();

                        AAlines.push_back("							<hkparam name=\"id\">" + eventID
                                          + "</hkparam>");
                        AAlines.push_back(
                            "							<hkparam name=\"payload\">null</hkparam>");
                        AAlines.push_back("						</hkobject>");
                        AAlines.push_back("					</hkparam>");

                        AAlines.push_back("					<hkparam name=\"relativeToEndOfClip\">"
                                          + string(negative ? "true" : "false") + "</hkparam>");

                        AAlines.push_back("					<hkparam name=\"acyclic\">false</hkparam>");
                        AAlines.push_back("					<hkparam name=\"isAnnotation\">false</hkparam>");
                        AAlines.push_back("				</hkobject>");

                        ++i;
                    }

                    if (error) throw nemesis::exception();

                    AAlines.push_back("			</hkparam>");
                    AAlines.push_back("		</hkobject>");
                    AAlines.push_back("");
                }

                if (error) throw nemesis::exception();
            }

            if (isChange) catalystMap[iter->first] = msglines;
        }

        DebugLogging(L"Processing behavior: " + filepath
                     + L" (Check point 4.4, AA count: " + to_wstring(clipAA.size()) + L" COMPLETE)");
    }

    VecStr PCEALines;

    if (pceaID.size() > 0)
    {
        DebugLogging(L"Processing behavior: " + filepath
                     + L" (Check point 4.6, PCEA count: " + to_wstring(pceaID.size()) + L")");
        unordered_map<string, int> replacerCount;

        for (auto& datalist : pceaID)
        {
            vector<VecStr> lineRe;
            string importline = to_string(datalist.first);

            if (error) throw nemesis::exception();

            for (auto data = datalist.second->rbegin(); data != datalist.second->rend(); ++data) {
                while (importline.length() < 4)
                {
                    importline = "0" + importline;
                }

                // populating manual selector generator
                VecStr msglines;
                msglines.push_back("		<hkobject name=\"#" + importline
                                   + "\" class=\"hkbManualSelectorGenerator\" signature=\"0xd932fab8\">");
                importline = "variableID[" + data->modFile + "]";
                variableIDReplacer(importline, "PCEA", behaviorFile, variableid, ZeroVariable, 0);

                if (exportID["variable_binding"]["selectedGeneratorIndex!~^!" + importline].length() == 0)
                {
                    string tempID = to_string(lastID);

                    while (tempID.length() < 4)
                    {
                        tempID = "0" + tempID;
                    }

                    exportID["variable_binding"]["selectedGeneratorIndex!~^!" + importline] = tempID;
                    importline = "			<hkparam name=\"variableBindingSet\">#" + tempID + "</hkparam>";
                    ++lastID;

                    if (lastID == 9216) ++lastID;
                }
                else
                {
                    importline = "			<hkparam name=\"variableBindingSet\">#"
                                 + exportID["variable_binding"]["selectedGeneratorIndex!~^!" + importline]
                                 + "</hkparam>";
                }

                msglines.push_back(importline);
                msglines.push_back("			<hkparam name=\"userData\">0</hkparam>");
                ++replacerCount[data->modFile];
                msglines.push_back("			<hkparam name=\"name\">Nemesis_PCEA_" + data->modFile
                                   + "_MSG</hkparam>");
                msglines.push_back("			<hkparam name=\"generators\" numelements=\"2\">");

                importline = to_string(lastID);

                while (importline.length() < 4)
                {
                    importline = "0" + importline;
                }

                msglines.push_back("				#" + importline);
                ++lastID;

                if (lastID == 9216) ++lastID;

                string baseID = to_string(lastID);

                while (baseID.length() < 4)
                {
                    baseID = "0" + baseID;
                }

                msglines.push_back("				#" + baseID);
                ++lastID;

                if (lastID == 9216) ++lastID;

                msglines.push_back("			</hkparam>");
                msglines.push_back("			<hkparam name=\"selectedGeneratorIndex\">0</hkparam>");
                msglines.push_back("			<hkparam name=\"currentGeneratorIndex\">0</hkparam>");
                msglines.push_back("		</hkobject>");
                msglines.push_back("");

                lineRe.push_back(msglines);
                msglines.clear();

                // populating clip generator
                msglines.push_back("		<hkobject name=\"#" + baseID
                                   + "\" class=\"hkbClipGenerator\" signature=\"0x333b85b9\">");
                msglines.push_back(catalystMap[datalist.first][1]);
                msglines.push_back(catalystMap[datalist.first][2]);
                msglines.push_back(catalystMap[datalist.first][3]);
                msglines.push_back("			<hkparam name=\"animationName\">" + data->animPath
                                   + "</hkparam>");

                for (unsigned int j = 5; j < catalystMap[datalist.first].size(); ++j)
                {
                    msglines.push_back(catalystMap[datalist.first][j]);
                }

                if (msglines.back().length() != 0) msglines.push_back("");

                if (error) throw nemesis::exception();

                lineRe.push_back(msglines);
            }

            VecStr msglines;
            msglines.push_back("		<hkobject name=\"#" + importline
                               + "\" class=\"hkbClipGenerator\" signature=\"0x333b85b9\">");
            msglines.insert(
                msglines.end(), catalystMap[datalist.first].begin() + 1, catalystMap[datalist.first].end());

            if (msglines.back().length() != 0) msglines.push_back("");

            lineRe.push_back(msglines);

            for (auto it = lineRe.rbegin(); it != lineRe.rend(); ++it) {
                PCEALines.insert(PCEALines.end(), it->begin(), it->end());
            }

            if (error) throw nemesis::exception();

            catalystMap.erase(catalystMap.find(datalist.first));
        }

        DebugLogging(L"Processing behavior: " + filepath + L" (Check point 4.8, PCEA count: "
                     + to_wstring(pceaID.size()) + L" COMPLETE)");
    }

    process->newMilestone();

    size_t reserveSize = 0;
    VecStr behaviorlines;

    // output import functions
    // Must be in vector
    vector<ImportContainer> groupExportID;
    groupExportID.push_back(exportID);
    VecStr additionallines = importOutput(groupExportID, 0, lastID);

    process->newMilestone();
    DebugLogging(L"Processing behavior: " + filepath + L" (Check point 5, Prepare to output)");
    process->newMilestone();

    if (behaviorPath[nemesis::transform_to<wstring>(lowerBehaviorFile)].size() == 0) ErrorMessage(1068, behaviorFile);

    wstring filename = getTempBhvrPath(nemesisInfo).wstring() + L"\\xml\\" + nemesis::transform_to<wstring>(modID + lowerBehaviorFile) + L".xml";

    redirToStageDir(outputdir, nemesisInfo);

    if (!FolderCreate(GetFileDirectory(filename)) || !FolderCreate(GetFileDirectory(outputdir))) return;

    ofstream output(filename);
    bool isClip = false;
    bool isBehavior = false;
    string clipName;
    wstring projectdir = outputdir.parent_path().parent_path().wstring();

    if (!output.is_open()) ErrorMessage(1025, filename);

    if (isCharacter)
    {
        for (int i = firstID; i < firstID + 4; ++i)
        {
            for (auto& eachline : catalystMap[i])
            {
                output << eachline + "\n";
                checkBehaviorJoint(outputdir, projectdir, eachline, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output
            << "<!-- ======================== NEMESIS import TEMPLATE START ======================== -->\n";

        if (additionallines.size() > 0)
        {
            output << "\n";

            for (auto& eachline : additionallines)
            {
                output << eachline + "\n";
                checkBehaviorJoint(outputdir, projectdir, eachline, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output
            << "<!-- ======================== NEMESIS import TEMPLATE END ======================== -->\n\n";
        output << "<!-- ======================== NEMESIS PCEA TEMPLATE START ======================== -->\n";

        if (PCEALines.size() > 0)
        {
            output << "\n";

            for (auto& eachline : PCEALines)
            {
                output << eachline + "\n";
                checkBehaviorJoint(outputdir, projectdir, eachline, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output << "<!-- ======================== NEMESIS PCEA TEMPLATE END ======================== -->\n\n";
        output << "<!-- ======================== NEMESIS alternate animation TEMPLATE START "
                  "======================== -->\n";

        if (AAlines.size() > 0)
        {
            output << "\n";

            for (auto& eachline : AAlines)
            {
                output << eachline + "\n";
                checkBehaviorJoint(outputdir, projectdir, eachline, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output << "<!-- ======================== NEMESIS alternate animation TEMPLATE END "
                  "======================== -->\n\n";

        for (unsigned int j = 0; j < allEditLines.size(); ++j)
        {
            for (auto& eachline : (*allEditLines[j]))
            {
                output << eachline + "\n";
                checkBehaviorJoint(outputdir, projectdir, eachline, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }
    }
    else
    {
        for (int i = firstID; i < firstID + 4; ++i)
        {
            for (auto& eachline : catalystMap[i])
            {
                output << eachline + "\n";
                checkClipAnimData(
                    outputdir, projectdir, eachline, characterFiles, clipName, isClip, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output
            << "<!-- ======================== NEMESIS import TEMPLATE START ======================== -->\n";

        if (additionallines.size() > 0)
        {
            output << "\n";

            for (auto& eachline : additionallines)
            {
                output << eachline + "\n";
                checkClipAnimData(
                    outputdir, projectdir, eachline, characterFiles, clipName, isClip, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output
            << "<!-- ======================== NEMESIS import TEMPLATE END ======================== -->\n\n";
        output << "<!-- ======================== NEMESIS PCEA TEMPLATE START ======================== -->\n";

        if (PCEALines.size() > 0)
        {
            output << "\n";

            for (auto& eachline : PCEALines)
            {
                output << eachline + "\n";
                checkClipAnimData(
                    outputdir, projectdir, eachline, characterFiles, clipName, isClip, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output << "<!-- ======================== NEMESIS PCEA TEMPLATE END ======================== -->\n\n";
        output << "<!-- ======================== NEMESIS alternate animation TEMPLATE START "
                  "======================== -->\n";

        if (AAlines.size() > 0)
        {
            output << "\n";

            for (auto& eachline : AAlines)
            {
                output << eachline + "\n";
                checkClipAnimData(
                    outputdir, projectdir, eachline, characterFiles, clipName, isClip, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }

        output << "<!-- ======================== NEMESIS alternate animation TEMPLATE END "
                  "======================== -->\n\n";

        for (unsigned int j = 0; j < allEditLines.size(); ++j)
        {
            for (auto& eachline : (*allEditLines[j]))
            {
                output << eachline + "\n";
                checkClipAnimData(
                    outputdir, projectdir, eachline, characterFiles, clipName, isClip, process, isBehavior);
            }

            if (error) throw nemesis::exception();
        }
    }

    firstID     = firstID + 4;
    size_t size = catalystMap.rbegin()->first + 1;

    for (size_t it = firstID; it < size; ++it)
    {
        for (auto& eachline : catalystMap[it])
        {
            output << eachline + "\n";
            checkClipAnimData(
                outputdir, projectdir, eachline, characterFiles, clipName, isClip, process, isBehavior);
        }

        if (error) throw nemesis::exception();
    }

    if (catalystMap.rbegin()->second.back().length() != 0) output << "\n";

    output << "	</hksection>\n\n";
    output << "</hkpackfile>\n";
    output.close();

    if (error) throw nemesis::exception();

    DebugLogging(L"Processing behavior: " + filepath + L" (Check point 6, Behavior output complete)");
    process->newMilestone();
    --extraCore;

    if (process->hkxCompiler.hkxcmdProcess(nemesis::transform_to<wstring>(filename), outputdir))
    {
        DebugLogging(L"Processing behavior: " + filepath + L" (Check point 7, Behavior compile complete)");
        process->newMilestone();
    }

    ++extraCore;
}

void BehaviorSub::addInfo(const wstring& newDirectory,
                          const string& curfile,
                          shared_ptr<TemplateInfo> newBehaviorTemplate,
                          unordered_map<string, vector<shared_ptr<NewAnimation>>> addAnimation,
                          unordered_map<string, var> newAnimVar,
                          mapSetString addAnimEvent,
                          mapSetString addAnimVariable,
                          unordered_map<string, unordered_map<int, bool>> newIgnoreFunction,
                          bool newIsCharacter,
                          const string& newModID,
                          BehaviorStart* newProcess)
{
    directory        = newDirectory;
    curfilefromlist  = curfile;
    BehaviorTemplate = newBehaviorTemplate;
    newAnimation     = addAnimation;
    AnimVar          = newAnimVar;
    newAnimEvent     = addAnimEvent;
    newAnimVariable  = addAnimVariable;
    ignoreFunction   = newIgnoreFunction;
    isCharacter      = newIsCharacter;
    modID            = newModID;
    process          = newProcess;
}

void BehaviorSub::checkAnimation()
{
    ++animCounter;

    if (animCounter >= base)
    {
        tryAddAnim = &BehaviorSub::addAnimation;

        if (animCounter > base) addAnimation();
    }
}

void BehaviorSub::addAnimation()
{
    process->increaseAnimCount();
}
