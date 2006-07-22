/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2004 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *                <omster@gmail.com>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include <sstream>
#include <string>
#include <deque>
#include <map>
#include <libpq-fe.h>

#include "users.h"
#include "channels.h"
#include "modules.h"
#include "helperfuncs.h"
#include "inspircd.h"
#include "configreader.h"

#include "m_sqlv2.h"

/* $ModDesc: PostgreSQL Service Provider module for all other m_sql* modules, uses v2 of the SQL API */
/* $CompileFlags: -I`pg_config --includedir` `perl extra/pgsql_config.pl` */
/* $LinkerFlags: -L`pg_config --libdir` -lpq */

/* UGH, UGH, UGH, UGH, UGH, UGH
 * I'm having trouble seeing how I
 * can avoid this. The core-defined
 * constructors for InspSocket just
 * aren't suitable...and if I'm
 * reimplementing them I need this so
 * I can access the socket engine :\
 */
extern InspIRCd* ServerInstance;
InspSocket* socket_ref[MAX_DESCRIPTORS];

/* Forward declare, so we can have the typedef neatly at the top */
class SQLConn;
/* Also needs forward declaration, as it's used inside SQLconn */
class ModulePgSQL;

typedef std::map<std::string, SQLConn*> ConnMap;

/* CREAD,	Connecting and wants read event
 * CWRITE,	Connecting and wants write event
 * WREAD,	Connected/Working and wants read event
 * WWRITE, 	Connected/Working and wants write event
 */
enum SQLstatus { CREAD, CWRITE, WREAD, WWRITE };

/** QueryQueue, a queue of queries waiting to be executed.
 * This maintains two queues internally, one for 'priority'
 * queries and one for less important ones. Each queue has
 * new queries appended to it and ones to execute are popped
 * off the front. This keeps them flowing round nicely and no
 * query should ever get 'stuck' for too long. If there are
 * queries in the priority queue they will be executed first,
 * 'unimportant' queries will only be executed when the
 * priority queue is empty.
 *
 * We store lists of SQLrequest's here, by value as we want to avoid storing
 * any data allocated inside the client module (in case that module is unloaded
 * while the query is in progress).
 *
 * Because we want to work on the current SQLrequest in-situ, we need a way
 * of accessing the request we are currently processing, QueryQueue::front(),
 * but that call needs to always return the same request until that request
 * is removed from the queue, this is what the 'which' variable is. New queries are
 * always added to the back of one of the two queues, but if when front()
 * is first called then the priority queue is empty then front() will return
 * a query from the normal queue, but if a query is then added to the priority
 * queue then front() must continue to return the front of the *normal* queue
 * until pop() is called.
 */

class QueryQueue : public classbase
{
private:
	typedef std::deque<SQLrequest> ReqDeque;	

	ReqDeque priority;	/* The priority queue */
	ReqDeque normal;	/* The 'normal' queue */
	enum { PRI, NOR, NON } which;	/* Which queue the currently active element is at the front of */

public:
	QueryQueue()
	: which(NON)
	{
	}
	
	void push(const SQLrequest &q)
	{
		log(DEBUG, "QueryQueue::push(): Adding %s query to queue: %s", ((q.pri) ? "priority" : "non-priority"), q.query.q.c_str());
		
		if(q.pri)
			priority.push_back(q);
		else
			normal.push_back(q);
	}
	
	void pop()
	{
		if((which == PRI) && priority.size())
		{
			priority.pop_front();
		}
		else if((which == NOR) && normal.size())
		{
			normal.pop_front();
		}
		
		/* Reset this */
		which = NON;
		
		/* Silently do nothing if there was no element to pop() */
	}
	
	SQLrequest& front()
	{
		switch(which)
		{
			case PRI:
				return priority.front();
			case NOR:
				return normal.front();
			default:
				if(priority.size())
				{
					which = PRI;
					return priority.front();
				}
				
				if(normal.size())
				{
					which = NOR;
					return normal.front();
				}
				
				/* This will probably result in a segfault,
				 * but the caller should have checked totalsize()
				 * first so..meh - moron :p
				 */
				
				return priority.front();
		}
	}
	
	std::pair<int, int> size()
	{
		return std::make_pair(priority.size(), normal.size());
	}
	
	int totalsize()
	{
		return priority.size() + normal.size();
	}
	
	void PurgeModule(Module* mod)
	{
		DoPurgeModule(mod, priority);
		DoPurgeModule(mod, normal);
	}
	
private:
	void DoPurgeModule(Module* mod, ReqDeque& q)
	{
		for(ReqDeque::iterator iter = q.begin(); iter != q.end(); iter++)
		{
			if(iter->GetSource() == mod)
			{
				if(iter->id == front().id)
				{
					/* It's the currently active query.. :x */
					iter->SetSource(NULL);
				}
				else
				{
					/* It hasn't been executed yet..just remove it */
					iter = q.erase(iter);
				}
			}
		}
	}
};

/** PgSQLresult is a subclass of the mostly-pure-virtual class SQLresult.
 * All SQL providers must create their own subclass and define it's methods using that
 * database library's data retriveal functions. The aim is to avoid a slow and inefficient process
 * of converting all data to a common format before it reaches the result structure. This way
 * data is passes to the module nearly as directly as if it was using the API directly itself.
 */

class PgSQLresult : public SQLresult
{
	PGresult* res;
	int currentrow;
	
	SQLfieldList* fieldlist;
	SQLfieldMap* fieldmap;
public:
	PgSQLresult(Module* self, Module* to, unsigned long id, PGresult* result)
	: SQLresult(self, to, id), res(result), currentrow(0), fieldlist(NULL), fieldmap(NULL)
	{
		int rows = PQntuples(res);
		int cols = PQnfields(res);
		
		log(DEBUG, "Created new PgSQL result; %d rows, %d columns", rows, cols);
	}
	
	~PgSQLresult()
	{
		PQclear(res);
	}
	
	virtual int Rows()
	{
		return PQntuples(res);
	}
	
	virtual int Cols()
	{
		return PQnfields(res);
	}
	
	virtual std::string ColName(int column)
	{
		char* name = PQfname(res, column);
		
		return (name) ? name : "";
	}
	
	virtual int ColNum(const std::string &column)
	{
		int n = PQfnumber(res, column.c_str());
		
		if(n == -1)
		{
			throw SQLbadColName();
		}
		else
		{
			return n;
		}
	}
	
	virtual SQLfield GetValue(int row, int column)
	{
		char* v = PQgetvalue(res, row, column);
		
		if(v)
		{
			return SQLfield(std::string(v, PQgetlength(res, row, column)), PQgetisnull(res, row, column));
		}
		else
		{
			log(DEBUG, "PQgetvalue returned a null pointer..nobody wants to tell us what this means");
			throw SQLbadColName();
		}
	}
	
	virtual SQLfieldList& GetRow()
	{
		/* In an effort to reduce overhead we don't actually allocate the list
		 * until the first time it's needed...so...
		 */
		if(fieldlist)
		{
			fieldlist->clear();
		}
		else
		{
			fieldlist = new SQLfieldList;
		}
		
		if(currentrow < PQntuples(res))
		{
			int cols = PQnfields(res);
			
			for(int i = 0; i < cols; i++)
			{
				fieldlist->push_back(GetValue(currentrow, i));
			}
			
			currentrow++;
		}
		
		return *fieldlist;
	}
	
	virtual SQLfieldMap& GetRowMap()
	{
		/* In an effort to reduce overhead we don't actually allocate the map
		 * until the first time it's needed...so...
		 */
		if(fieldmap)
		{
			fieldmap->clear();
		}
		else
		{
			fieldmap = new SQLfieldMap;
		}
		
		if(currentrow < PQntuples(res))
		{
			int cols = PQnfields(res);
			
			for(int i = 0; i < cols; i++)
			{
				fieldmap->insert(std::make_pair(ColName(i), GetValue(currentrow, i)));
			}
			
			currentrow++;
		}
		
		return *fieldmap;
	}
	
	virtual SQLfieldList* GetRowPtr()
	{
		SQLfieldList* fl = new SQLfieldList;
		
		if(currentrow < PQntuples(res))
		{
			int cols = PQnfields(res);
			
			for(int i = 0; i < cols; i++)
			{
				fl->push_back(GetValue(currentrow, i));
			}
			
			currentrow++;
		}
		
		return fl;
	}
	
	virtual SQLfieldMap* GetRowMapPtr()
	{
		SQLfieldMap* fm = new SQLfieldMap;
		
		if(currentrow < PQntuples(res))
		{
			int cols = PQnfields(res);
			
			for(int i = 0; i < cols; i++)
			{
				fm->insert(std::make_pair(ColName(i), GetValue(currentrow, i)));
			}
			
			currentrow++;
		}
		
		return fm;
	}
	
	virtual void Free(SQLfieldMap* fm)
	{
		DELETE(fm);
	}
	
	virtual void Free(SQLfieldList* fl)
	{
		DELETE(fl);
	}
};

/** SQLConn represents one SQL session.
 * Each session has its own persistent connection to the database.
 * This is a subclass of InspSocket so it can easily recieve read/write events from the core socket
 * engine, unlike the original MySQL module this module does not block. Ever. It gets a mild stabbing
 * if it dares to.
 */

class SQLConn : public InspSocket
{
private:
	ModulePgSQL* us;		/* Pointer to the SQL provider itself */
	Server* Srv;			/* Server* for..uhm..something, maybe */
	std::string 	dbhost;	/* Database server hostname */
	unsigned int	dbport;	/* Database server port */
	std::string 	dbname;	/* Database name */
	std::string 	dbuser;	/* Database username */
	std::string 	dbpass;	/* Database password */
	bool			ssl;	/* If we should require SSL */
	PGconn* 		sql;	/* PgSQL database connection handle */
	SQLstatus		status;	/* PgSQL database connection status */
	bool			qinprog;/* If there is currently a query in progress */
	QueryQueue		queue;	/* Queue of queries waiting to be executed on this connection */

public:

	/* This class should only ever be created inside this module, using this constructor, so we don't have to worry about the default ones */

	SQLConn(ModulePgSQL* self, Server* srv, const std::string &h, unsigned int p, const std::string &d, const std::string &u, const std::string &pwd, bool s);

	~SQLConn();

	bool DoResolve();

	bool DoConnect();

	virtual void Close();
	
	bool DoPoll();
	
	bool DoConnectedPoll();
	
	void ShowStatus();	
	
	virtual bool OnDataReady();

	virtual bool OnWriteReady();
	
	virtual bool OnConnected();
	
	bool DoEvent();
	
	std::string MkInfoStr();
	
	const char* StatusStr();
	
	SQLerror DoQuery(SQLrequest &req);
	
	SQLerror Query(const SQLrequest &req);
	
	void OnUnloadModule(Module* mod);
};

class ModulePgSQL : public Module
{
private:
	Server* Srv;
	ConnMap connections;
	unsigned long currid;
	char* sqlsuccess;

public:
	ModulePgSQL(Server* Me)
	: Module::Module(Me), Srv(Me), currid(0)
	{
		log(DEBUG, "%s 'SQL' feature", Srv->PublishFeature("SQL", this) ? "Published" : "Couldn't publish");
		log(DEBUG, "%s 'PgSQL' feature", Srv->PublishFeature("PgSQL", this) ? "Published" : "Couldn't publish");
		
		sqlsuccess = new char[strlen(SQLSUCCESS)+1];
		
		strcpy(sqlsuccess, SQLSUCCESS);

		OnRehash("");
	}

	void Implements(char* List)
	{
		List[I_OnUnloadModule] = List[I_OnRequest] = List[I_OnRehash] = List[I_OnUserRegister] = List[I_OnCheckReady] = List[I_OnUserDisconnect] = 1;
	}

	virtual void OnRehash(const std::string &parameter)
	{
		ConfigReader conf;
		
		/* Delete all the SQLConn objects in the connection lists,
		 * this will call their destructors where they can handle
		 * closing connections and such.
		 */
		for(ConnMap::iterator iter = connections.begin(); iter != connections.end(); iter++)
		{
			DELETE(iter->second);
		}
		
		/* Empty out our list of connections */
		connections.clear();

		for(int i = 0; i < conf.Enumerate("database"); i++)
		{
			std::string id;
			SQLConn* newconn;
			
			id = conf.ReadValue("database", "id", i);
			newconn = new SQLConn(this, Srv,
										conf.ReadValue("database", "hostname", i),
										conf.ReadInteger("database", "port", i, true),
										conf.ReadValue("database", "name", i),
										conf.ReadValue("database", "username", i),
										conf.ReadValue("database", "password", i),
										conf.ReadFlag("database", "ssl", i));
			
			connections.insert(std::make_pair(id, newconn));
		}	
	}
	
	virtual char* OnRequest(Request* request)
	{
		if(strcmp(SQLREQID, request->GetData()) == 0)
		{
			SQLrequest* req = (SQLrequest*)request;
			ConnMap::iterator iter;
		
			log(DEBUG, "Got query: '%s' with %d replacement parameters on id '%s'", req->query.q.c_str(), req->query.p.size(), req->dbid.c_str());

			if((iter = connections.find(req->dbid)) != connections.end())
			{
				/* Execute query */
				req->id = NewID();
				req->error = iter->second->Query(*req);
				
				return (req->error.Id() == NO_ERROR) ? sqlsuccess : NULL;
			}
			else
			{
				req->error.Id(BAD_DBID);
				return NULL;
			}
		}

		log(DEBUG, "Got unsupported API version string: %s", request->GetData());
		
		return NULL;
	}
	
	virtual void OnUnloadModule(Module* mod, const std::string&	name)
	{
		/* When a module unloads we have to check all the pending queries for all our connections
		 * and set the Module* specifying where the query came from to NULL. If the query has already
		 * been dispatched then when it is processed it will be dropped if the pointer is NULL.
		 *
		 * If the queries we find are not already being executed then we can simply remove them immediately.
		 */
		for(ConnMap::iterator iter = connections.begin(); iter != connections.end(); iter++)
		{
			
		}
	}

	unsigned long NewID()
	{
		if (currid+1 == 0)
			currid++;
		
		return ++currid;
	}
		
	virtual Version GetVersion()
	{
		return Version(1, 0, 0, 0, VF_VENDOR|VF_SERVICEPROVIDER);
	}
	
	virtual ~ModulePgSQL()
	{
		DELETE(sqlsuccess);
	}	
};

SQLConn::SQLConn(ModulePgSQL* self, Server* srv, const std::string &h, unsigned int p, const std::string &d, const std::string &u, const std::string &pwd, bool s)
: InspSocket::InspSocket(), us(self), Srv(srv), dbhost(h), dbport(p), dbname(d), dbuser(u), dbpass(pwd), ssl(s), sql(NULL), status(CWRITE), qinprog(false)
{
	log(DEBUG, "Creating new PgSQL connection to database %s on %s:%u (%s/%s)", dbname.c_str(), dbhost.c_str(), dbport, dbuser.c_str(), dbpass.c_str());

	/* Some of this could be reviewed, unsure if I need to fill 'host' etc...
	 * just copied this over from the InspSocket constructor.
	 */
	strlcpy(this->host, dbhost.c_str(), MAXBUF);
	this->port = dbport;
	
	this->ClosePending = false;
	
	if(!inet_aton(this->host, &this->addy))
	{
		/* Its not an ip, spawn the resolver.
		 * PgSQL doesn't do nonblocking DNS 
		 * lookups, so we do it for it.
		 */
		
		log(DEBUG,"Attempting to resolve %s", this->host);
		
		this->dns.SetNS(Srv->GetConfig()->DNSServer);
		this->dns.ForwardLookupWithFD(this->host, fd);
		
		this->state = I_RESOLVING;
		socket_ref[this->fd] = this;
		
		return;
	}
	else
	{
		log(DEBUG,"No need to resolve %s", this->host);
		strlcpy(this->IP, this->host, MAXBUF);
		
		if(!this->DoConnect())
		{
			throw ModuleException("Connect failed");
		}
	}
}

SQLConn::~SQLConn()
{
	Close();
}

bool SQLConn::DoResolve()
{	
	log(DEBUG, "Checking for DNS lookup result");
	
	if(this->dns.HasResult())
	{
		std::string res_ip = dns.GetResultIP();
		
		if(res_ip.length())
		{
			log(DEBUG, "Got result: %s", res_ip.c_str());
			
			strlcpy(this->IP, res_ip.c_str(), MAXBUF);
			dbhost = res_ip;
			
			socket_ref[this->fd] = NULL;
			
			return this->DoConnect();
		}
		else
		{
			log(DEBUG, "DNS lookup failed, dying horribly");
			Close();
			return false;
		}
	}
	else
	{
		log(DEBUG, "No result for lookup yet!");
		return true;
	}
}

bool SQLConn::DoConnect()
{
	log(DEBUG, "SQLConn::DoConnect()");
	
	if(!(sql = PQconnectStart(MkInfoStr().c_str())))
	{
		log(DEBUG, "Couldn't allocate PGconn structure, aborting: %s", PQerrorMessage(sql));
		Close();
		return false;
	}
	
	if(PQstatus(sql) == CONNECTION_BAD)
	{
		log(DEBUG, "PQconnectStart failed: %s", PQerrorMessage(sql));
		Close();
		return false;
	}
	
	ShowStatus();
	
	if(PQsetnonblocking(sql, 1) == -1)
	{
		log(DEBUG, "Couldn't set connection nonblocking: %s", PQerrorMessage(sql));
		Close();
		return false;
	}
	
	/* OK, we've initalised the connection, now to get it hooked into the socket engine
	 * and then start polling it.
	 */
	
	log(DEBUG, "Old DNS socket: %d", this->fd);
	this->fd = PQsocket(sql);
	log(DEBUG, "New SQL socket: %d", this->fd);
	
	if(this->fd <= -1)
	{
		log(DEBUG, "PQsocket says we have an invalid FD: %d", this->fd);
		Close();
		return false;
	}
	
	this->state = I_CONNECTING;
	ServerInstance->SE->AddFd(this->fd,false,X_ESTAB_MODULE);
	socket_ref[this->fd] = this;
	
	/* Socket all hooked into the engine, now to tell PgSQL to start connecting */
	
	return DoPoll();
}

void SQLConn::Close()
{
	log(DEBUG,"SQLConn::Close");
	
	if(this->fd > 01)
		socket_ref[this->fd] = NULL;
	this->fd = -1;
	this->state = I_ERROR;
	this->OnError(I_ERR_SOCKET);
	this->ClosePending = true;
	
	if(sql)
	{
		PQfinish(sql);
		sql = NULL;
	}
	
	return;
}

bool SQLConn::DoPoll()
{
	switch(PQconnectPoll(sql))
	{
		case PGRES_POLLING_WRITING:
			log(DEBUG, "PGconnectPoll: PGRES_POLLING_WRITING");
			WantWrite();
			status = CWRITE;
			return DoPoll();
		case PGRES_POLLING_READING:
			log(DEBUG, "PGconnectPoll: PGRES_POLLING_READING");
			status = CREAD;
			break;
		case PGRES_POLLING_FAILED:
			log(DEBUG, "PGconnectPoll: PGRES_POLLING_FAILED: %s", PQerrorMessage(sql));
			return false;
		case PGRES_POLLING_OK:
			log(DEBUG, "PGconnectPoll: PGRES_POLLING_OK");
			status = WWRITE;
			return DoConnectedPoll();
		default:
			log(DEBUG, "PGconnectPoll: wtf?");
			break;
	}
	
	return true;
}

bool SQLConn::DoConnectedPoll()
{
	if(!qinprog && queue.totalsize())
	{
		/* There's no query currently in progress, and there's queries in the queue. */
		SQLrequest& query = queue.front();
		DoQuery(query);
	}
	
	if(PQconsumeInput(sql))
	{
		log(DEBUG, "PQconsumeInput succeeded");
			
		if(PQisBusy(sql))
		{
			log(DEBUG, "Still busy processing command though");
		}
		else if(qinprog)
		{
			log(DEBUG, "Looks like we have a result to process!");
			
			/* Grab the request we're processing */
			SQLrequest& query = queue.front();
			
			log(DEBUG, "ID is %lu", query.id);
			
			/* Get a pointer to the module we're about to return the result to */
			Module* to = query.GetSource();
			
			/* Fetch the result.. */
			PGresult* result = PQgetResult(sql);
			
			/* PgSQL would allow a query string to be sent which has multiple
			 * queries in it, this isn't portable across database backends and
			 * we don't want modules doing it. But just in case we make sure we
			 * drain any results there are and just use the last one.
			 * If the module devs are behaving there will only be one result.
			 */
			while (PGresult* temp = PQgetResult(sql))
			{
				PQclear(result);
				result = temp;
			}
			
			if(to)
			{
				/* ..and the result */
				log(DEBUG, "Got result, status code: %s; error message: %s", PQresStatus(PQresultStatus(result)), PQresultErrorMessage(result));
					
				PgSQLresult reply(us, to, query.id, result);
				
				reply.Send();
				
				/* PgSQLresult's destructor will free the PGresult */
			}
			else
			{
				/* If the client module is unloaded partway through a query then the provider will set
				 * the pointer to NULL. We cannot just cancel the query as the result will still come
				 * through at some point...and it could get messy if we play with invalid pointers...
				 */
				log(DEBUG, "Looks like we're handling a zombie query from a module which unloaded before it got a result..fun. ID: %lu", query.id);
				PQclear(result);
			}
			
			qinprog = false;
			queue.pop();				
			DoConnectedPoll();
		}
		
		return true;
	}
	
	log(DEBUG, "PQconsumeInput failed: %s", PQerrorMessage(sql));
	return false;
}

void SQLConn::ShowStatus()
{
	switch(PQstatus(sql))
	{
		case CONNECTION_STARTED:
			log(DEBUG, "PQstatus: CONNECTION_STARTED: Waiting for connection to be made.");
			break;

		case CONNECTION_MADE:
			log(DEBUG, "PQstatus: CONNECTION_MADE: Connection OK; waiting to send.");
			break;
		
		case CONNECTION_AWAITING_RESPONSE:
			log(DEBUG, "PQstatus: CONNECTION_AWAITING_RESPONSE: Waiting for a response from the server.");
			break;
		
		case CONNECTION_AUTH_OK:
			log(DEBUG, "PQstatus: CONNECTION_AUTH_OK: Received authentication; waiting for backend start-up to finish.");
			break;
		
		case CONNECTION_SSL_STARTUP:
			log(DEBUG, "PQstatus: CONNECTION_SSL_STARTUP: Negotiating SSL encryption.");
			break;
		
		case CONNECTION_SETENV:
			log(DEBUG, "PQstatus: CONNECTION_SETENV: Negotiating environment-driven parameter settings.");
			break;
		
		default:
			log(DEBUG, "PQstatus: ???");
	}
}

bool SQLConn::OnDataReady()
{
	/* Always return true here, false would close the socket - we need to do that ourselves with the pgsql API */
	log(DEBUG, "OnDataReady(): status = %s", StatusStr());
	
	return DoEvent();
}

bool SQLConn::OnWriteReady()
{
	/* Always return true here, false would close the socket - we need to do that ourselves with the pgsql API */
	log(DEBUG, "OnWriteReady(): status = %s", StatusStr());
	
	return DoEvent();
}

bool SQLConn::OnConnected()
{
	log(DEBUG, "OnConnected(): status = %s", StatusStr());
	
	return DoEvent();
}

bool SQLConn::DoEvent()
{
	bool ret;
	
	if((status == CREAD) || (status == CWRITE))
	{
		ret = DoPoll();
	}
	else
	{
		ret = DoConnectedPoll();
	}
	
	switch(PQflush(sql))
	{
		case -1:
			log(DEBUG, "Error flushing write queue: %s", PQerrorMessage(sql));
			break;
		case 0:
			log(DEBUG, "Successfully flushed write queue (or there was nothing to write)");
			break;
		case 1:
			log(DEBUG, "Not all of the write queue written, triggering write event so we can have another go");
			WantWrite();
			break;
	}

	return ret;
}

std::string SQLConn::MkInfoStr()
{			
	std::ostringstream conninfo("connect_timeout = '2'");
	
	if(dbhost.length())
		conninfo << " hostaddr = '" << dbhost << "'";
	
	if(dbport)
		conninfo << " port = '" << dbport << "'";
	
	if(dbname.length())
		conninfo << " dbname = '" << dbname << "'";
	
	if(dbuser.length())
		conninfo << " user = '" << dbuser << "'";
	
	if(dbpass.length())
		conninfo << " password = '" << dbpass << "'";
	
	if(ssl)
		conninfo << " sslmode = 'require'";
	
	return conninfo.str();
}

const char* SQLConn::StatusStr()
{
	if(status == CREAD) return "CREAD";
	if(status == CWRITE) return "CWRITE";
	if(status == WREAD) return "WREAD";
	if(status == WWRITE) return "WWRITE";
	return "Err...what, erm..BUG!";
}

SQLerror SQLConn::DoQuery(SQLrequest &req)
{
	if((status == WREAD) || (status == WWRITE))
	{
		if(!qinprog)
		{
			/* Parse the command string and dispatch it */
			
			/* Pointer to the buffer we screw around with substitution in */
			char* query;
			/* Pointer to the current end of query, where we append new stuff */
			char* queryend;
			/* Total length of the unescaped parameters */
			unsigned int paramlen;
			
			paramlen = 0;
			
			for(ParamL::iterator i = req.query.p.begin(); i != req.query.p.end(); i++)
			{
				paramlen += i->size();
			}
			
			/* To avoid a lot of allocations, allocate enough memory for the biggest the escaped query could possibly be.
			 * sizeofquery + (totalparamlength*2) + 1
			 * 
			 * The +1 is for null-terminating the string for PQsendQuery()
			 */
			
			query = new char[req.query.q.length() + (paramlen*2)];
			queryend = query;
			
			/* Okay, now we have a buffer large enough we need to start copying the query into it and escaping and substituting
			 * the parameters into it...
			 */
			
			for(unsigned int i = 0; i < req.query.q.length(); i++)
			{
				if(req.query.q[i] == '?')
				{
					/* We found a place to substitute..what fun.
					 * Use the PgSQL calls to escape and write the
					 * escaped string onto the end of our query buffer,
					 * then we "just" need to make sure queryend is
					 * pointing at the right place.
					 */
					
					if(req.query.p.size())
					{
						int error = 0;
						size_t len = 0;

#ifdef PGSQL_HAS_ESCAPECONN
						len = PQescapeStringConn(sql, queryend, req.query.p.front().c_str(), req.query.p.front().length(), &error);
#else
						len = PQescapeStringConn(queryend, req.query.p.front().c_str(), req.query.p.front().length());
						error = 0;
#endif
						
						if(error)
						{
							log(DEBUG, "Apparently PQescapeStringConn() failed somehow...don't know how or what to do...");
						}
						
						log(DEBUG, "Appended %d bytes of escaped string onto the query", len);
						
						/* Incremenet queryend to the end of the newly escaped parameter */
						queryend += len;
						
						/* Remove the parameter we just substituted in */
						req.query.p.pop_front();
					}
					else
					{
						log(DEBUG, "Found a substitution location but no parameter to substitute :|");
						break;
					}
				}
				else
				{
					*queryend = req.query.q[i];
					queryend++;
				}
			}
			
			/* Null-terminate the query */
			*queryend = 0;
	
			log(DEBUG, "Attempting to dispatch query: %s", query);
			
			req.query.q = query;

			if(PQsendQuery(sql, query))
			{
				log(DEBUG, "Dispatched query successfully");
				qinprog = true;
				DELETE(query);
				return SQLerror();
			}
			else
			{
				log(DEBUG, "Failed to dispatch query: %s", PQerrorMessage(sql));
				DELETE(query);
				return SQLerror(QSEND_FAIL, PQerrorMessage(sql));
			}
		}
	}

	log(DEBUG, "Can't query until connection is complete");
	return SQLerror(BAD_CONN, "Can't query until connection is complete");
}

SQLerror SQLConn::Query(const SQLrequest &req)
{
	queue.push(req);
	
	if(!qinprog && queue.totalsize())
	{
		/* There's no query currently in progress, and there's queries in the queue. */
		SQLrequest& query = queue.front();
		return DoQuery(query);
	}
	else
	{
		return SQLerror();
	}
}

void SQLConn::OnUnloadModule(Module* mod)
{
	queue.PurgeModule(mod);
}

class ModulePgSQLFactory : public ModuleFactory
{
 public:
	ModulePgSQLFactory()
	{
	}
	
	~ModulePgSQLFactory()
	{
	}
	
	virtual Module * CreateModule(Server* Me)
	{
		return new ModulePgSQL(Me);
	}
};


extern "C" void * init_module( void )
{
	return new ModulePgSQLFactory;
}
