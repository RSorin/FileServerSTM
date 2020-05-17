/**
 *  Radu Sorin-Gabriel
 *  Grupa 410 - M1
 **/

#include <iostream>
#include <thread>
#include <cstdlib>
#include <chrono>
#include <random>
#include <ctime>
#include <string>
#include "stm/stm.h"
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <sstream>
#include <fstream>
#include <regex>
#include <mutex>
#include <shared_mutex>
#include <map>
#define PORT 8080

using namespace std;
using namespace stm;

const char *GREEN = "\033[0;32m";
const char *RESET = "\033[0m";

// Mutex-uri pentru ReadWriteLock
typedef std::shared_mutex Lock;
typedef std::unique_lock<Lock> WriteLock;
typedef std::shared_lock<Lock> ReadLock;

Context ctx;
// Mutex-uri pentru logging si pentru vector-ul de lock-uri corespunzatoare
// fiecarui client
std::mutex logLock;
std::mutex insertLocksLock;
struct CommandRt
{
    std::mutex &logLock;
    Context &context;
    // Nodul aferent primului path din comanda  
    TVar<struct Node> tfirstNode;
    // Nodul aferent celui de-al doilea path din comanda
    TVar<struct Node> tsecondNode;
};
// Resursa pentru notificari
struct NotificationsRt
{
    Context &context;
    // Aici sunt clientii cu notificari (id-ul lor si vectorul de mesaje aferent)
    TVar <vector<struct notificationMessages>* > tclientsWithNotifications;
};
// Resursa pentru path-urile restrictionate
struct RestrictedRt
{
    Context &context;
    TVar<vector<string>*> tRestrictedPaths;
};
struct Node
{
    // Bucata de cale curenta
    // (pentru /etc/hosts ar fi etc sa zicem)
    string path;
    // Leg urmatoarea bucata de cale cu nodul aferent
    map<string, TVar<struct Node> >* nextNode;
    bool operator ==(const struct Node& n)
    {
        return path == n.path;
    }

    bool operator !=(const struct Node &n)
    {
        return path != n.path;
    }

    struct Node& operator =(const struct Node& n)
    {
        path = n.path;
        nextNode = new map<string, TVar<struct Node> >;
        *(n.nextNode) = *(n.nextNode);
        return *this;
    }
    
};

// Structura de fisiere
struct File
{
    // Calea completa
    string fullPath;
    // Succesiunea de noduri aferenta caii
    struct Node* node;
};
// Structura pentru raspuns
// cod + mesaj
struct Response
{
    string code;
    string message;
};
class SendThread;
// Fiecare client cu notificari are:
// ID-ul
// Clasa executata de thread-ul clientului care se ocupa cu
// trimiterea efectiva a notificarilor
// Un vector cu comenzile date pe server (mesajele de notificare) 
struct notificationMessages
{
    string clientID;
    SendThread* snd;
    vector<string> messages;
    bool operator !=(const struct notificationMessages& n)
    {
        return clientID.compare(n.clientID) != 0 && messages != n.messages;
    }
    bool operator ==(const struct notificationMessages &n)
    {
        return clientID.compare(n.clientID) == 0 && messages == n.messages;
    }
};

// Vectorul de notificari pe baza caruia se face TVar-ul
vector<struct notificationMessages> clientsWithNotification;
// Mutex pentru sistemul de fisiere (vecotrul cu cai)
mutex fileSystemLock;
// Vectorul de cai restrictionate pe baza caruia se face TVar-ul
vector<string> restrictedPaths;
// Sistemul de fisiere (ca vector de cai)
vector<struct File> fileSystem;
// Lock-uri pentru fiecare bucata cale (bucata cale -> Lock)
map<string, Lock&> locks;
// Functie pentru logging
void log(string message)
{
    logLock.lock();
    cout << GREEN << message << RESET << endl;
    logLock.unlock();
}
// Functie pentru generarea unui ID random
string generate_id(int max_length)
{
    string possible_characters = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    random_device rd;
    mt19937 engine(rd());
    uniform_int_distribution<> dist(0, possible_characters.size() - 1);
    string ret = "";
    for (int i = 0; i < max_length; i++)
    {
        int random_index = dist(engine);
        ret += possible_characters[random_index];
    }
    return ret;
}
// Functie pentru split-uirea unui string
vector<string> split(string s, string delimiter)
{
    vector<string> list;
    size_t pos = 0;
    string token;
    while ((pos = s.find(delimiter)) != string::npos)
    {
        token = s.substr(0, pos);
        list.push_back(token);
        s.erase(0, pos + delimiter.length());
    }
    list.push_back(s);
    return list;
}
// Functie care cauta un client in resursa de notificari
// (TVar cu vector de notificari)
struct notificationMessages *findNotifications(string clientID, Context &ctx, TVar<vector<struct notificationMessages> *> notif)
{
    vector<struct notificationMessages> *aux = atomically(ctx, readTVar(notif));
    for (int i = 0; i < aux->size(); ++i)
        if ((*aux)[i].clientID.compare(clientID) == 0)
            return &(*aux)[i];
    return NULL;
}
// Functie care cauta o cale in resursa de cai restrictionata
// (TVar cu vector de cai)
bool search(string path, TVar<vector<string>*> restricted)
{
    vector<string> *aux = atomically(ctx, readTVar(restricted));
    for (int i=0;i<aux->size();++i)
        if (path.rfind((*aux)[i], 0) == 0)
            return true;
    return false;
}
// Verificare regex pentru comenzi
bool validateInput(string command)
{
    regex re("^exit|START_NOTIFICATIONS|END_NOTIFICATIONS$|^(CREATE_FOLDER|READ_FILE|DELETE|START_MODIFICATIONS|STOP_MODIFICATIONS) [A-Za-z0-9/_~.]+$|^WRITE_FILE [A-Za-z0-9/_~.]+ .*$|^MOVE [A-Za-z0-9/_~.]+ [A-Za-z0-9/~_.]+$");
    return regex_match(command, re);
}
// Functie care face bind recursiv
// Primeste un TVar de Node si returneaza un STML de Node
STML<Node> recursiveBind(const TVar<Node> &node,bool needsWriteLock=false)
{
    // Citesc nodul curent din TVar si il leg cu urmatorul
    return bind<struct Node, struct Node>( 
        readTVar(node),
        [&](const struct Node& n)
        {
            const string c = n.path;
            // Lock-urile sunt cele corespunzatoare fiecarei cai
            // Iau resursa cu un ReadLock
            ReadLock((locks.find(c))->second);
            if (n.nextNode == NULL)
            {
                // Daca este ultimul nod din cale si am o comanda de genul
                // WRITE_FILE, iau resursa ultimului nod cu WriteLock
                if (needsWriteLock)
                    WriteLock((locks.find(c))->second);
                return pure<Node>(n);
            }
            // Extrag nodul urmator din nodul curent
            struct Node& no = const_cast<struct Node&>(n);
            map<string, TVar<struct Node> > aux = *(no.nextNode);
            TVar<Node> nextNode = aux[no.path];
            // Continui bind-ul recursiv cu nodul extras
            return recursiveBind(nextNode, needsWriteLock);
        });
}

// Functie care executa o comanda pe server
STML<Response> executeOperation(const vector<TVar<Node> > &nodes, string request, int socket, string clientID, TVar<vector<struct notificationMessages>* > notif, TVar<vector<string>* > restricted, SendThread* snd)
{
    // Iau TVar-urile aferente cailor din comanda
    // In functie de tipul comenzii, pot fi: 0, 1 sau 2 cai
    // adica TVar-uri cu noduri
    TVar<Node> firstnode;
    if (nodes.size() > 0)
        firstnode = nodes[0];
    TVar<Node> secondnode;
    if (nodes.size() > 1)
        secondnode = nodes[1];
    // Fac split comenzii ca sa pot lua calea
    vector<string> splitCommand;
    if (request.find(" ") != string::npos)
        splitCommand = split(request, " ");
    else
        splitCommand.push_back(request);
    // O variabila dummy pentru a functiona bind-urile
    struct Node dummy;
    STML<Node> bindResult = pure<struct Node>(dummy);
    if (splitCommand.size() > 1)
    {
        // Daca comanda contine 2 cai, trebuie sa iau resursele pentru ambele
        // si fac bind intre resursa pentru prima si cea pentru a doua
        if (nodes.size() > 1)
        {   bool needsWrite = false;
            // Daca comanda e MOVE am nevoie de WriteLock pe ambele cai
            if (splitCommand[0].compare("MOVE") == 0)
                needsWrite = true;
            bindResult = bind<Node, Node>(
                        recursiveBind(firstnode, needsWrite),
                        [secondnode, needsWrite](const Node& n)
                        {
                            return recursiveBind(secondnode, needsWrite);
                        });
        }
        else
        {
            // Daca comanda este WRITE_FILE, trebuie ca ultima resursa de tip nod
            // sa fie luata cu WriteLock
            if (splitCommand[0].compare("WRITE_FILE") == 0)
                bindResult = recursiveBind(firstnode, true);
            else
                bindResult = recursiveBind(firstnode, false);
        }
    }
    // Daca comanda este pentru activarea notificarilor, pun clientul
    // in vectorul de clienti cu notificari (modificand resursa aferenta)
    if (splitCommand[0].compare("START_NOTIFICATIONS") == 0)
    {
        bindResult = bind<Node, Node>(
            bindResult,
            [notif, clientID,snd](const Node& n)
            {
                STML<Unit> modified = modifyTVar<vector<struct notificationMessages> *>(notif, [clientID, snd](vector<struct notificationMessages> *result) {
                    struct notificationMessages messages;
                    messages.clientID = clientID;
                    messages.snd = snd;
                    vector<string> m;
                    messages.messages = m;
                    result->push_back(messages);
                    return result;
                });
                return bind<Unit, struct Node>(
                    modified,
                    [&](const Unit &) { return pure<Node>(n); });
               
            });
    }
    // Daca comanda este pentru oprirea notificarilor, scot clientul
    // din vectorul de clienti cu notificari (modificand resursa aferenta)
    if (splitCommand[0].compare("END_NOTIFICATIONS") == 0)
    {
        bindResult = bind<Node, Node>(
            bindResult,
            [notif, clientID](const struct Node &n) {
                STML<Unit> modified = modifyTVar<vector<struct notificationMessages> *>(notif, [notif,clientID](vector<struct notificationMessages> *result) {
                    for (int i=0;i<result->size();++i)
                        if ((*result)[i].clientID.compare(clientID) == 0)
                            result->erase(remove(result->begin(), result->end(), (*result)[i]), result->end());
                    return result;
                });
                return bind<Unit, struct Node>(
                    modified,
                    [&](const Unit &) { return pure<Node>(n); });
            });
    }
    // Daca comanda este pentru activarea modificarilor, scot calea repsectiva
    // din vectorul de cai restrictionate (modificand resursa aferenta)
    if (splitCommand[0].compare("START_MODIFICATIONS") == 0)
    {
        string rPath = splitCommand[1];
        bindResult = bind<Node, Node>(
            bindResult,
            [restricted, rPath](const struct Node& n) {
                STML<Unit> modified = modifyTVar<vector<string>*>(restricted, [restricted, rPath](vector<string>* restr) {
                    restr->erase(remove(restr->begin(), restr->end(), rPath), restr->end());
                    return restr;
                });
                return bind<Unit, struct Node>(
                    modified,
                    [&](const Unit &) { return pure<Node>(n); });
            });
    }
    // Daca comanda este pentru dezactivarea modificarilor, adaug calea repsectiva
    // in vectorul de cai restrictionate (modificand resursa aferenta)
    if (splitCommand[0].compare("STOP_MODIFICATIONS") == 0)
    {
        string rPath = splitCommand[1];
        bindResult = bind<Node, Node>(
            bindResult,
            [restricted, rPath](const struct Node& n) {
                STML<Unit> modified = modifyTVar<vector<string> *>(restricted, [restricted, rPath](vector<string> *restr) {
                    restr->push_back(rPath);
                    return restr;
                });
                return bind<Unit, struct Node>(
                    modified,
                    [&](const Unit &) { return pure<Node>(n); });
            });
    }
    // Bind-urile anterioare au fost facute pentru a lega tranzactiile intr-una singura
    // Bind-ul final pentru raspuns
    return bind<Node, Response>(
        bindResult,
        [socket, clientID, request, restricted](const struct Node& node) {
            //  Split-uiesc comanda ca sa iau caile si comanda in sine
            vector<string> splitCommand;
            if (request.find(" ") != string::npos)
                splitCommand = split(request, " ");
            else
                splitCommand.push_back(request);
            string command = splitCommand[0];
            // Pentru fiecare actiune returnez raspunsul si fac log la operatia pe server
            if (command.compare("START_MODIFICATIONS") == 0)
            {
                log("Modification are now allowed on path " + splitCommand[1]);
                Response response;
                response.code = "OK";
                response.message = "MODIFICATIONS ARE NOW ALLOWED ON " + splitCommand[1] + "\n";
                return pure<Response>(response);
            }
            if (command.compare("STOP_MODIFICATIONS") == 0)
            {
                log("Modification are now not allowed on path " + splitCommand[1]);
                Response response;
                response.code = "OK";
                response.message = "MODIFICATIONS ARE NOW NOT ALLOWED ON " + splitCommand[1] + "\n";
                return pure<Response>(response);
            }
            if (command.compare("START_NOTIFICATIONS") == 0)
            {
                log("Activate notifications for client " + clientID);
                Response response;
                response.code = "OK";
                response.message = "ACTIVATE NOTIFICATIONS\n";
                return pure<Response>(response);
            }
            if (command.compare("END_NOTIFICATIONS") == 0)
            {
                log("Deactivate notifications for client " + clientID);
                Response response;
                response.code = "OK";
                response.message = "DEACTIVATE NOTIFICATIONS\n";
                return pure<Response>(response);
            }
            if (splitCommand.size() > 1)
            {
                // Ma uit sa vad daca operatia este permisa pe calea respectiva
                // (daca nu cumva calea este pusa in vectorul de cai restrictionate)
                if (command.compare("READ_FILE") != 0)
                {
                    bool sres = false;
                    if (command.compare("MOVE") == 0)
                        sres = search(splitCommand[2], restricted);
                    if (sres || search(splitCommand[1], restricted))
                    {
                        Response response;
                        response.code = "ERROR";
                        response.message = "STOP_MODIFICATIONS ACTIVE ON THIS FILE\n";
                        return pure<Response>(response);
                    }
                }
            }
            // Construiesc comanda care va fi executata efectiv (in shell)
            string toExecute;
            if (command.compare("CREATE_FOLDER") == 0)
                toExecute = "mkdir -p " + splitCommand[1];
            if (command.compare("READ_FILE") == 0)
            {
                toExecute = "cat " + splitCommand[1];
            }
            if (command.compare("WRITE_FILE") == 0)
            {
                string content = "";
                for (int i = 2; i < splitCommand.size(); ++i)
                    content += splitCommand[i];
                toExecute = "echo " + content + " > " + splitCommand[1];
            }
            if (command.compare("DELETE") == 0)
                toExecute = "rm -rf " + splitCommand[1];
            if (command.compare("MOVE") == 0)
                toExecute = "mv " + splitCommand[1] + " " + splitCommand[2];
            string fName = "status_" + clientID + ".txt";
            // Apelez script-ul care imi da si rezultatul comenzii + statusul ei
            // Aceste informatii sunt scrise intr-un fisier separat pentru fiecare client
            string ex = "./execute_client_command \"" + toExecute + "\" \"" + fName + "\"";

            //Execut script-ul
            system(ex.c_str());

            // Citesc rezultatul si statusul comenzii din fisier
            ifstream f(fName);
            string message = "";
            string status;
            string lastLine;
            while (!f.eof())
            {
                string line;
                getline(f, line);
                if (f.eof())
                {
                    status = lastLine;
                    message = message.substr(0, message.size()-(lastLine.size()+1));
                }
                else
                    message += (line + "\n");
                lastLine = line;
            }
            // Sterg fisierul cu rezultatul
            system(("rm -rf status_" + clientID + ".txt").c_str());
            stringstream statusString(status);
            int statusCode;
            statusString >> statusCode;
            // Returnez raspunsul comenzii
            Response response;
            if (statusCode != 0)
                response.code = "ERROR";
            else
                response.code = "OK";
            response.message = message;
            return pure<Response>(response);
        });
}

// Clasa care este executata de thread-ul care trimite notificari clientului
class SendThread
{
    // Notificarea care trebuie trimisa
    string *toSend;
    bool exit;
    mutex mtx;
    condition_variable cv;

public:
    SendThread(string *s, bool ex)
    {
        toSend = s;
        exit = ex;
    }

    // Opresc thread-ul si trimit o notificare dummy ca sa iasa din wait
    void setExit()
    {
        exit = true;
        prepareToSend("dummy");
        return;
    }

    // Setez notificarea si trezesc thread-ul care trimite notificari
    void prepareToSend(string str)
    {
        unique_lock<mutex> lck(mtx);
        string *st = new string(str);
        toSend = st;
        cv.notify_one();
        return;
    }

    void execute(int socket, string clientID, const NotificationsRt& notif)
    {
        // Cat timp nu inchid thread-ul
        while (!exit)
        {
            unique_lock<mutex> lck(mtx);
            // Astept ca cineva sa puna o notificare de trimis catre clientul
            // de care ma ocup
            while (toSend == NULL)
                cv.wait(lck);
            // Termin executia daca am primit semnalul de exit
            if (exit)
                break;
            // Daca clientul de care ma ocup are notificari care asteapta sa fie trimise,
            // le trimit acestuia si curat vecotrul de mesaje aferent clientului de care ma ocup
            notificationMessages *mess = findNotifications(clientID, notif.context, notif.tclientsWithNotifications);
            if ((mess != NULL) && (mess->messages.size() > 0))
            {
                notificationMessages messages = *mess;
                log("Sending notifications ... ");
                string notifToSend("NOTIFICATIONS:\n");
                for (int i = 0; i < messages.messages.size(); ++i)
                    notifToSend += (messages.messages[i] + "\n");
                if (notifToSend.compare("NOTIFICATIONS:\n") != 0)
                    send(socket, notifToSend.data(), notifToSend.size(), 0);
                TVar<vector<struct notificationMessages> *> noti = notif.tclientsWithNotifications;
                STML<Unit> modified = modifyTVar<vector<struct notificationMessages> *>(noti, [noti, clientID](vector<struct notificationMessages> *result) {
                    for (int i = 0; i < result->size(); ++i)
                        if ((*result)[i].clientID.compare(clientID) == 0)
                            (*result)[i].messages.clear();
                    return result;
                });
                atomically(ctx, modified);
            }
            // Trimit si rezultatul comenzii daca este cazul
            if (toSend->compare("dummy") != 0)
            {
                string st = *toSend;
                send(socket, st.data(), st.size(), 0);
            }
            // Resetez ca sa pot primi iar semnale
            delete toSend;
            toSend = NULL;
        }
        return;
    }
};

// Functie care pune comanda data ca argument in vectorul de mesaje de notificare
// al fiecarui client
// Totodata anunt thread-urile clientilor ca pot trimite notificari
void notificateClient(const char* command, string clientID, Context& ctx, TVar<vector<struct notificationMessages>* > notif)
{
    string str(command);
    STML<Unit> modified = modifyTVar<vector<struct notificationMessages> *>(notif, [str, clientID](vector<struct notificationMessages> *result) {
        for (int i=0;i<result->size();++i)
        {
            if ((*result)[i].clientID.compare(clientID) != 0)
            {
                (*result)[i].messages.push_back(str);
                ((*result)[i].snd)->prepareToSend("dummy");
            }
        }
        return result;
    });
    atomically(ctx, modified);
}
// Functie care imi gaseste o cale in resursa sistemului de fisiere
struct Node* searchInFileSystem(string path)
{
    for (int i=0;i< fileSystem.size();++i)
        if (fileSystem[i].fullPath.compare(path) == 0)
            return fileSystem[i].node;
    return NULL;
}
// Functie care construieste lantul de noduri din cale
struct Node* constructNodes(vector<string> path)
{
    // Aloc memore pentru nod
    struct Node *b = new struct Node;
    // Pun bucata de cale in el
    b->path = path[0];
    Lock *l = new Lock();
    // Adaug calea si un lock in map-ul de lock-uri
    insertLocksLock.lock();
    locks.insert(pair<string, Lock&>(path[0], *l));
    insertLocksLock.unlock();
    // Daca am ajuns la ultimul nod din care, urmatorul e NULL
    if (path.size() == 1)
        b->nextNode = NULL;
    else
    {
        // Daca nu, sterg bucata de cale curenta din vector
        // Construiesc apoi map-ul pentru urmatorul nod
        // Apelez recursiv constructia urmatorului nod
        string aux = path[0];
        path.erase(path.begin());
        map<string, TVar<struct Node> >* aux_ptr = new map<string, TVar<struct Node> >;
        map<string, TVar<struct Node> > aux_deref = *aux_ptr;
        (*aux_ptr)[aux] = newTVarIO(ctx, *constructNodes(path));
        // Atasez map-ul construit nodului curent
        b->nextNode = aux_ptr;
    }
    // Returnez nodul nou alocat si construit
    return b;
}

// Functie pentru servirea unui client
// Este apelata de un thread
// Un thread se creeaza pentru fiecare client si executa functia aceasta
void serveClient(int socket, string clientID, const NotificationsRt &notif, const RestrictedRt &restr)
{
    // Pronesc thread-ul care trimite notificari aferent clientului
    SendThread* th = new SendThread(NULL, false); 
    thread sendThread(&SendThread::execute, th, socket, clientID, ref(notif));
    while(1)
    {
        // Citesc comanda trimisa de client
        char buffer[1024] = {0};
        int valread = read(socket, buffer, 1024);
        // Daca socket-ul s-a inchis, ies din while
        if (valread <= 0)
            break;
        // Verific daca comanda e buna
        string request(buffer);
        if (!validateInput(request))
        {
            string res = "ERROR: INVALID COMMAND\n";
            send(socket, res.c_str(), strlen(res.c_str()), 0);
            continue;
        }
        // Daca comanda e exit, termin executia thread-ului
        if (request.compare("exit") == 0)
            break;
        // Split-uiesc comanda
        vector<string> splitCommand = split(request, " ");
        string command = splitCommand[0];
        struct Node* fs;
        struct Node* sec;
        // Daca comanda necesita, construiesc unul sau doua lanturi de noduri
        // Si le pun in sistemul de fisiere
        if (splitCommand.size() > 1)
        {
            if ((fs = searchInFileSystem(splitCommand[1])) == NULL)
            {
                fs = new struct Node;
                vector<string> path = split(splitCommand[1], "/");
                if (path[0].compare("") == 0)
                    path.erase(path.begin());
                fs = constructNodes(path);
                struct File file;
                file.fullPath = splitCommand[1];
                file.node = fs;
                fileSystemLock.lock();
                fileSystem.push_back(file);
                fileSystemLock.unlock();
            }
            if (command.compare("MOVE") == 0)
                if ((sec = searchInFileSystem(splitCommand[2])) == NULL)
                {
                    sec = new struct Node;
                    sec = constructNodes(split(splitCommand[2], "/"));
                    struct File file;
                    file.fullPath = splitCommand[2];
                    file.node = sec;
                    fileSystemLock.lock();
                    fileSystem.push_back(file);
                    fileSystemLock.unlock();
                }
        }
        // Pun lanturile de noduri construite in TVar-uri
        TVar<Node> firstNode;
        if (fs != NULL)
        {
            firstNode = newTVarIO(ctx, *fs);
        }
        TVar<Node> secondNode;
        if (sec != NULL) 
            secondNode = newTVarIO(ctx, *sec);

        // Pregatesc TVar-urile pentru executia comenzii
        CommandRt rt = {logLock, ctx, firstNode, secondNode};
        vector<TVar<Node> > nodes;
        if (fs != NULL)
            nodes.push_back(rt.tfirstNode);
        if (sec != NULL)
            nodes.push_back(rt.tsecondNode);
        // Execut comanda ca o tranzactie pe server
        Response response = atomically(rt.context, executeOperation(nodes, request, socket, clientID, notif.tclientsWithNotifications, restr.tRestrictedPaths, th));
        // Fac log la comanda
        log("Client with id " + clientID + " executes the following command: " + request + " ########## Status: " + response.code + ((response.code == "ERROR") ? ("; Reason: " + response.message) : ""));
        string res = response.code + ":\n" + response.message + "\0";
        // Anunt thread-ul care trimite notificari la client ca trebuie
        // sa trimita rezultatul unei comenzi
        th->prepareToSend(res);

        // Daca comanda nu este START_MODIFICATIONS sau END_NOTIFICATIONS
        // notific toti clientii care au activate notificarile de faptul ca
        // comanda respectiva s-a executat pe server
        if ((command.compare("START_MODIFICATIONS") != 0)
            &&(command.compare("END_NOTIFICATIONS") != 0))
                notificateClient(request.c_str(), clientID, rt.context, notif.tclientsWithNotifications);
    }
    // Inchid thread-ul de notificari
    th->setExit();
    sendThread.join();
    delete th;
    // Inchid socket-ul clientului
    close(socket);
    log("Client with id " + clientID + " left the server");
    // Sterg clientul din lista de clienti care au notificarile activate
    notificationMessages *aux = findNotifications(clientID, ctx, notif.tclientsWithNotifications);
    if (aux != NULL)
    {
        TVar<vector<struct notificationMessages> *> noti = notif.tclientsWithNotifications;
        STML<Unit> modified = modifyTVar<vector<struct notificationMessages> *>(noti, [noti, clientID](vector<struct notificationMessages> *result) {
            for (int i = 0; i < result->size(); ++i)
                if ((*result)[i].clientID.compare(clientID) == 0)
                    result->erase(remove(result->begin(), result->end(), (*result)[i]), result->end());
            return result;
        });
        atomically(ctx, modified);
    }
    return;
}

int main()
{
    int server_fd, new_socket, valread;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // Creez socket-ul server-ului
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Setez sochet-ul sa reutilizeze adresa IP si portul
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt)))
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Atasez socket-ul pe portul 8080
    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    log("Server started on port 8080");
    // Ascult pentru maxim 10 clienti care se pot conecta concomitent
    if (listen(server_fd, 10) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    // Creez TVar-urile si resursele
    TVar<vector<struct notificationMessages>*> notifications = newTVarIO(ctx, new vector<struct notificationMessages>);
    TVar<vector<string>*> restricted = newTVarIO(ctx, new vector<string>);
    NotificationsRt rtNotif = {ctx, notifications};
    RestrictedRt rtRestrict = {ctx, restricted};
    while(1)
    {
        log("Server listening...");
        // Accept conexiuni de la clienti
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
                                (socklen_t *)&addrlen)) < 0)
        {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        // Generez un ID random de 32 de caractere pentru client
        string clientID = generate_id(32);
        log("Client with id " + clientID + " connected to server");
        
        // Pornesc un nou thread care sa se ocupe de clientul nou conectat
        thread servingThread(serveClient, new_socket, clientID, rtNotif, rtRestrict);
        servingThread.detach();
    }
    // Inchid file descriptor-ul socket-ului server-ului
    close(server_fd);
    return 0;
}
