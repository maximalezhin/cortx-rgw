// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MDS.h"
#include "Server.h"
#include "Locker.h"
#include "MDCache.h"
#include "MDLog.h"
#include "Migrator.h"
#include "MDBalancer.h"
#include "Renamer.h"

#include "msg/Messenger.h"

#include "messages/MClientMount.h"
#include "messages/MClientMountAck.h"
#include "messages/MClientRequest.h"
#include "messages/MClientReply.h"

#include "messages/MLock.h"

#include "messages/MDentryUnlink.h"
#include "messages/MInodeLink.h"

#include "events/EString.h"
#include "events/EUpdate.h"
#include "events/EMount.h"

#include "include/filepath.h"
#include "common/Timer.h"
#include "common/Logger.h"
#include "common/LogType.h"

#include <errno.h>
#include <fcntl.h>

#include <list>
#include <iostream>
using namespace std;

#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) cout << g_clock.now() << " mds" << mds->get_nodeid() << ".server "
#define  derr(l)    if (l<=g_conf.debug || l <= g_conf.debug_mds) cout << g_clock.now() << " mds" << mds->get_nodeid() << ".server "


void Server::dispatch(Message *m) 
{
  // active?
  if (!mds->is_active()) {
    dout(3) << "not active yet, waiting" << endl;
    mds->queue_waitfor_active(new C_MDS_RetryMessage(mds, m));
    return;
  }

  switch (m->get_type()) {
  case MSG_CLIENT_MOUNT:
    handle_client_mount((MClientMount*)m);
    return;
  case MSG_CLIENT_UNMOUNT:
    handle_client_unmount(m);
    return;
  case MSG_CLIENT_REQUEST:
    handle_client_request((MClientRequest*)m);
    return;

  }

  dout(1) << " main unknown message " << m->get_type() << endl;
  assert(0);
}



// ----------------------------------------------------------
// MOUNT and UNMOUNT


class C_MDS_mount_finish : public Context {
  MDS *mds;
  Message *m;
  bool mount;
  version_t cmapv;
public:
  C_MDS_mount_finish(MDS *m, Message *msg, bool mnt, version_t mv) :
    mds(m), m(msg), mount(mnt), cmapv(mv) { }
  void finish(int r) {
    assert(r == 0);

    // apply
    if (mount)
      mds->clientmap.add_mount(m->get_source_inst());
    else
      mds->clientmap.rem_mount(m->get_source().num());
    
    assert(cmapv == mds->clientmap.get_version());
    
    // reply
    if (mount) {
      // mounted
      mds->messenger->send_message(new MClientMountAck((MClientMount*)m, mds->mdsmap, mds->osdmap), 
				   m->get_source_inst());
      delete m;
    } else {
      // ack by sending back to client
      mds->messenger->send_message(m, m->get_source_inst());

      // unmounted
      if (g_conf.mds_shutdown_on_last_unmount &&
	  mds->clientmap.get_mount_set().empty()) {
	dout(3) << "all clients done, initiating shutdown" << endl;
	mds->shutdown_start();
      }
    }
  }
};


void Server::handle_client_mount(MClientMount *m)
{
  dout(3) << "mount by " << m->get_source() << " oldv " << mds->clientmap.get_version() << endl;

  // journal it
  version_t cmapv = mds->clientmap.inc_projected();
  mdlog->submit_entry(new EMount(m->get_source_inst(), true, cmapv),
		      new C_MDS_mount_finish(mds, m, true, cmapv));
}

void Server::handle_client_unmount(Message *m)
{
  dout(3) << "unmount by " << m->get_source() << " oldv " << mds->clientmap.get_version() << endl;

  // journal it
  version_t cmapv = mds->clientmap.inc_projected();
  mdlog->submit_entry(new EMount(m->get_source_inst(), false, cmapv),
		      new C_MDS_mount_finish(mds, m, false, cmapv));
}



/*******
 * some generic stuff for finishing off requests
 */

/** C_MDS_CommitRequest
 */

class C_MDS_CommitRequest : public Context {
  Server *server;
  MClientRequest *req;
  MClientReply *reply;
  CInode *tracei;    // inode to include a trace for
  LogEvent *event;

public:
  C_MDS_CommitRequest(Server *server,
                      MClientRequest *req, MClientReply *reply, CInode *tracei, 
                      LogEvent *event=0) {
    this->server = server;
    this->req = req;
    this->tracei = tracei;
    this->reply = reply;
    this->event = event;
  }
  void finish(int r) {
    if (r != 0) {
      // failure.  set failure code and reply.
      reply->set_result(r);
    }
    if (event) {
      server->commit_request(req, reply, tracei, event);
    } else {
      // reply.
      server->reply_request(req, reply, tracei);
    }
  }
};


/*
 * send generic response (just and error code)
 */
void Server::reply_request(MClientRequest *req, int r, CInode *tracei)
{
  reply_request(req, new MClientReply(req, r), tracei);
}


/*
 * send given reply
 * include a trace to tracei
 */
void Server::reply_request(MClientRequest *req, MClientReply *reply, CInode *tracei) {
  dout(10) << "reply_request r=" << reply->get_result() << " " << *req << endl;

  // include trace
  if (tracei) {
    reply->set_trace_dist( tracei, mds->get_nodeid() );
  }
  
  // send reply
  messenger->send_message(reply,
                          req->get_client_inst());

  // discard request
  mdcache->request_finish(req);

  // stupid stats crap (FIXME)
  stat_ops++;
}


void Server::submit_update(MClientRequest *req,
			   CInode *wrlockedi,
			   LogEvent *event,
			   Context *oncommit)
{
  // log
  mdlog->submit_entry(event);

  // pin
  mdcache->request_pin_inode(req, wrlockedi);

  // wait
  mdlog->wait_for_sync(oncommit);
}


/* 
 * commit event(s) to the metadata journal, then reply.
 * or, be sloppy and do it concurrently (see g_conf.mds_log_before_reply)
 *
 * NOTE: this is old and bad (write-behind!)
 */
void Server::commit_request(MClientRequest *req,
                         MClientReply *reply,
                         CInode *tracei,
                         LogEvent *event,
                         LogEvent *event2) 
{      
  // log
  if (event) mdlog->submit_entry(event);
  if (event2) mdlog->submit_entry(event2);
  
  if (g_conf.mds_log_before_reply && g_conf.mds_log && event) {
    // SAFE mode!

    // pin inode so it doesn't go away!
    if (tracei) mdcache->request_pin_inode(req, tracei);

    // wait for log sync
    mdlog->wait_for_sync(new C_MDS_CommitRequest(this, req, reply, tracei)); 
    return;
  }
  else {
    // just reply
    reply_request(req, reply, tracei);
  }
}



/***
 * process a client request
 */

void Server::handle_client_request(MClientRequest *req)
{
  dout(4) << "req " << *req << endl;

  if (!mds->is_active()) {
    dout(5) << " not active, discarding client request." << endl;
    delete req;
    return;
  }
  
  if (!mdcache->get_root()) {
    dout(5) << "need to open root" << endl;
    mdcache->open_root(new C_MDS_RetryMessage(mds, req));
    return;
  }

  // okay, i want
  CInode           *ref = 0;
  vector<CDentry*> trace;      // might be blank, for fh guys

  bool follow_trailing_symlink = false;

  // operations on fh's or other non-files
  switch (req->get_op()) {
    /*
  case MDS_OP_FSTAT:
    reply = handle_client_fstat(req, cur);
    break; ****** fiX ME ***
    */
    
  case MDS_OP_TRUNCATE:
    if (!req->args.truncate.ino) break;   // can be called w/ either fh OR path
    
  case MDS_OP_RELEASE:
  case MDS_OP_FSYNC:
    ref = mdcache->get_inode(req->args.fsync.ino);   // fixme someday no ino needed?

    if (!ref) {
      int next = mds->get_nodeid() + 1;
      if (next >= mds->mdsmap->get_num_mds()) next = 0;
      dout(10) << "got request on ino we don't have, passing buck to " << next << endl;
      mds->send_message_mds(req, next, MDS_PORT_SERVER);
      return;
    }
  }

  if (!ref) {
    // we need to traverse a path
    filepath refpath = req->get_filepath();
    
    // ops on non-existing files --> directory paths
    switch (req->get_op()) {
    case MDS_OP_OPEN:
      if (!(req->args.open.flags & O_CREAT)) break;
      
    case MDS_OP_MKNOD:
    case MDS_OP_MKDIR:
    case MDS_OP_SYMLINK:
    case MDS_OP_LINK:
    case MDS_OP_UNLINK:   // also wrt parent dir, NOT the unlinked inode!!
    case MDS_OP_RMDIR:
    case MDS_OP_RENAME:
      // remove last bit of path
      refpath = refpath.prefixpath(refpath.depth()-1);
      break;
    }
    dout(10) << "refpath = " << refpath << endl;
    
    Context *ondelay = new C_MDS_RetryMessage(mds, req);
    
    if (req->get_op() == MDS_OP_LSTAT) {
      follow_trailing_symlink = false;
    }

    // do trace
    int r = mdcache->path_traverse(refpath, trace, follow_trailing_symlink,
                                   req, ondelay,
                                   MDS_TRAVERSE_FORWARD,
                                   0,
                                   true); // is MClientRequest
    
    if (r > 0) return; // delayed
    if (r == -ENOENT ||
        r == -ENOTDIR ||
        r == -EISDIR) {
      // error! 
      dout(10) << " path traverse error " << r << ", replying" << endl;
      
      // send error
      messenger->send_message(new MClientReply(req, r),
                              req->get_client_inst());

      // <HACK>
      // is this a special debug command?
      if (refpath.depth() - 1 == trace.size() &&
	  refpath.last_dentry().find(".ceph.") == 0) {
	/*
FIXME dirfrag
	CDir *dir = 0;
	if (!trace.empty()) 
	  dir = mdcache->get_root()->dir;
	else
	  dir = trace[trace.size()-1]->get_inode()->dir;

	dout(1) << "** POSSIBLE CEPH DEBUG COMMAND '" << refpath.last_dentry() << "' in " << *dir << endl;

	if (refpath.last_dentry() == ".ceph.hash" &&
	    refpath.depth() > 1) {
	  dout(1) << "got explicit hash command " << refpath << endl;
	  /// ....
	}
	else if (refpath.last_dentry() == ".ceph.commit") {
	  dout(1) << "got explicit commit command on  " << *dir << endl;
	  dir->commit(0, 0);
	}
*/
      }
      // </HACK>


      delete req;
      return;
    }
    
    if (trace.size()) 
      ref = trace[trace.size()-1]->inode;
    else
      ref = mdcache->get_root();
  }
  
  dout(10) << "ref is " << *ref << endl;
  
  // rename doesn't pin src path (initially)
  if (req->get_op() == MDS_OP_RENAME) trace.clear();

  // register
  if (!mdcache->request_start(req, ref, trace))
    return;
  
  // process
  dispatch_request(req, ref);
}



void Server::dispatch_request(Message *m, CInode *ref)
{
  MClientRequest *req = 0;

  // MLock or MClientRequest?
  /* this is a little weird.
     client requests and mlocks both initial dentry xlocks, path pins, etc.,
     and thus both make use of the context C_MDS_RetryRequest.
  */
  switch (m->get_type()) {
  case MSG_CLIENT_REQUEST:
    req = (MClientRequest*)m;
    break; // continue below!

  case MSG_MDS_LOCK:
    mds->locker->handle_lock_dn((MLock*)m);
    return; // done

  default:
    assert(0);  // shouldn't get here
  }

  // MClientRequest.

  switch (req->get_op()) {
    
    // files
  case MDS_OP_OPEN:
    if (req->args.open.flags & O_CREAT) 
      handle_client_openc(req, ref);
    else 
      handle_client_open(req, ref);
    break;
  case MDS_OP_TRUNCATE:
    handle_client_truncate(req, ref);
    break;
    /*
  case MDS_OP_FSYNC:
    handle_client_fsync(req, ref);
    break;
    */
    /*
  case MDS_OP_RELEASE:
    handle_client_release(req, ref);
    break;
    */

    // inodes
  case MDS_OP_STAT:
  case MDS_OP_LSTAT:
    handle_client_stat(req, ref);
    break;
  case MDS_OP_UTIME:
    handle_client_utime(req, ref);
    break;
  case MDS_OP_CHMOD:
    handle_client_chmod(req, ref);
    break;
  case MDS_OP_CHOWN:
    handle_client_chown(req, ref);
    break;

    // namespace
  case MDS_OP_READDIR:
    handle_client_readdir(req, ref);
    break;
  case MDS_OP_MKNOD:
    handle_client_mknod(req, ref);
    break;
  case MDS_OP_LINK:
    handle_client_link(req, ref);
    break;
  case MDS_OP_UNLINK:
    handle_client_unlink(req, ref);
    break;
  case MDS_OP_RENAME:
    handle_client_rename(req, ref);
    break;
  case MDS_OP_RMDIR:
    handle_client_unlink(req, ref);
    break;
  case MDS_OP_MKDIR:
    handle_client_mkdir(req, ref);
    break;
  case MDS_OP_SYMLINK:
    handle_client_symlink(req, ref);
    break;



  default:
    dout(1) << " unknown client op " << req->get_op() << endl;
    assert(0);
  }

  return;
}


// FIXME: this probably should go somewhere else.

CDir* Server::try_open_dir(CInode *in, frag_t fg, MClientRequest *req)
{
  CDir *dir = in->get_dirfrag(fg);
  if (dir) 
    return dir; 

  if (in->is_frozen_dir()) {
    dout(10) << "try_open_dir: dir inode is frozen, waiting " << *in << endl;
    assert(in->get_parent_dir());
    in->get_parent_dir()->add_waiter(CDir::WAIT_UNFREEZE,
                                     new C_MDS_RetryRequest(mds, req, in));
    return 0;
  }

  return in->get_or_open_dirfrag(mds->mdcache, fg);
}

CDir* Server::try_open_auth_dir(CInode *diri, frag_t fg, MClientRequest *req)
{
  CDir *dir = diri->get_dirfrag(fg);

  // not open and inode not mine?
  if (!dir && !diri->is_auth()) {
    int inauth = diri->authority().first;
    dout(7) << "try_open_auth_dir: not open, not inode auth, fw to mds" << inauth << endl;
    mdcache->request_forward(req, inauth);
    return 0;
  }

  // not open and inode frozen?
  if (!dir && diri->is_frozen_dir()) {
    dout(10) << "try_open_dir: dir inode is frozen, waiting " << *diri << endl;
    assert(diri->get_parent_dir());
    diri->get_parent_dir()->add_waiter(CDir::WAIT_UNFREEZE,
				       new C_MDS_RetryRequest(mds, req, diri));
    return 0;
  }

  // invent?
  if (!dir) {
    assert(diri->is_auth());
    dir = diri->get_or_open_dirfrag(mds->mdcache, fg);
  }
  assert(dir);
 
  // am i auth for the dirfrag?
  if (!dir->is_auth()) {
    int auth = dir->authority().first;
    dout(7) << "try_open_auth_dir: not auth for " << *dir
	    << ", fw to mds" << auth << endl;
    mdcache->request_forward(req, auth);
    return 0;
  }

  return dir;
}



// ===============================================================================
// STAT

void Server::handle_client_stat(MClientRequest *req,
				CInode *ref)
{
  // FIXME: this is really not the way to handle the statlite mask.

  // do I need file info?
  int mask = req->args.stat.mask;
  if (mask & (INODE_MASK_SIZE|INODE_MASK_MTIME)) {
    // yes.  do a full stat.
    if (!mds->locker->inode_file_read_start(ref, req))
      return;  // syncing
    mds->locker->inode_file_read_finish(ref);
  } else {
    // nope!  easy peasy.
  }
  
  mds->balancer->hit_inode(ref, META_POP_IRD);   
  
  // reply
  //dout(10) << "reply to " << *req << " stat " << ref->inode.mtime << endl;
  MClientReply *reply = new MClientReply(req);
  reply_request(req, reply, ref);
}




// ===============================================================================
// INODE UPDATES


/* 
 * finisher: do a inode_file_write_finish and reply.
 */
class C_MDS_utime_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CInode *in;
  version_t pv;
  time_t mtime, atime;
public:
  C_MDS_utime_finish(MDS *m, MClientRequest *r, CInode *i, version_t pdv, time_t mt, time_t at) :
    mds(m), req(r), in(i), 
    pv(pdv),
    mtime(mt), atime(at) { }
  void finish(int r) {
    assert(r == 0);

    // apply
    in->inode.mtime = mtime;
    in->inode.atime = atime;
    in->mark_dirty(pv);

    // unlock
    mds->locker->inode_file_write_finish(in);

    // reply
    MClientReply *reply = new MClientReply(req, 0);
    reply->set_result(0);
    mds->server->reply_request(req, reply, in);
  }
};


// utime

void Server::handle_client_utime(MClientRequest *req,
				 CInode *cur)
{
  // write
  if (!mds->locker->inode_file_write_start(cur, req))
    return;  // fw or (wait for) sync

  mds->balancer->hit_inode(cur, META_POP_IWR);   

  // prepare
  version_t pdv = cur->pre_dirty();
  time_t mtime = req->args.utime.modtime;
  time_t atime = req->args.utime.actime;
  C_MDS_utime_finish *fin = new C_MDS_utime_finish(mds, req, cur, pdv, 
						   mtime, atime);

  // log + wait
  EUpdate *le = new EUpdate("utime");
  le->metablob.add_dir_context(cur->get_parent_dir());
  inode_t *pi = le->metablob.add_dentry(cur->parent, true);
  pi->mtime = mtime;
  pi->atime = mtime;
  pi->ctime = g_clock.gettime();
  pi->version = pdv;
  
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}


// --------------

/* 
 * finisher: do a inode_hard_write_finish and reply.
 */
class C_MDS_chmod_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CInode *in;
  version_t pv;
  int mode;
public:
  C_MDS_chmod_finish(MDS *m, MClientRequest *r, CInode *i, version_t pdv, int mo) :
    mds(m), req(r), in(i), pv(pdv), mode(mo) { }
  void finish(int r) {
    assert(r == 0);

    // apply
    in->inode.mode &= ~04777;
    in->inode.mode |= (mode & 04777);
    in->mark_dirty(pv);

    // unlock
    mds->locker->inode_hard_write_finish(in);

    // reply
    MClientReply *reply = new MClientReply(req, 0);
    reply->set_result(0);
    mds->server->reply_request(req, reply, in);
  }
};


// chmod

void Server::handle_client_chmod(MClientRequest *req,
				 CInode *cur)
{
  // write
  if (!mds->locker->inode_hard_write_start(cur, req))
    return;  // fw or (wait for) lock

  mds->balancer->hit_inode(cur, META_POP_IWR);   

  // prepare
  version_t pdv = cur->pre_dirty();
  int mode = req->args.chmod.mode;
  C_MDS_chmod_finish *fin = new C_MDS_chmod_finish(mds, req, cur, pdv,
						   mode);

  // log + wait
  EUpdate *le = new EUpdate("chmod");
  le->metablob.add_dir_context(cur->get_parent_dir());
  inode_t *pi = le->metablob.add_dentry(cur->parent, true);
  pi->mode = mode;
  pi->version = pdv;
  pi->ctime = g_clock.gettime();
  
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}


// chown

class C_MDS_chown_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CInode *in;
  version_t pv;
  int uid, gid;
public:
  C_MDS_chown_finish(MDS *m, MClientRequest *r, CInode *i, version_t pdv, int u, int g) :
    mds(m), req(r), in(i), pv(pdv), uid(u), gid(g) { }
  void finish(int r) {
    assert(r == 0);

    // apply
    if (uid >= 0) in->inode.uid = uid;
    if (gid >= 0) in->inode.gid = gid;
    in->mark_dirty(pv);

    // unlock
    mds->locker->inode_hard_write_finish(in);

    // reply
    MClientReply *reply = new MClientReply(req, 0);
    reply->set_result(0);
    mds->server->reply_request(req, reply, in);
  }
};


void Server::handle_client_chown(MClientRequest *req,
				 CInode *cur)
{
  // write
  if (!mds->locker->inode_hard_write_start(cur, req))
    return;  // fw or (wait for) lock

  mds->balancer->hit_inode(cur, META_POP_IWR);   

  // prepare
  version_t pdv = cur->pre_dirty();
  int uid = req->args.chown.uid;
  int gid = req->args.chown.gid;
  C_MDS_chown_finish *fin = new C_MDS_chown_finish(mds, req, cur, pdv,
						   uid, gid);

  // log + wait
  EUpdate *le = new EUpdate("chown");
  le->metablob.add_dir_context(cur->get_parent_dir());
  inode_t *pi = le->metablob.add_dentry(cur->parent, true);
  if (uid >= 0) pi->uid = uid;
  if (gid >= 0) pi->gid = gid;
  pi->version = pdv;
  pi->ctime = g_clock.gettime();
  
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}







// =================================================================
// DIRECTORY and NAMESPACE OPS

// READDIR

int Server::encode_dir_contents(CDir *dir, 
				list<InodeStat*>& inls,
				list<string>& dnls)
{
  int numfiles = 0;

  for (CDir_map_t::iterator it = dir->begin(); 
       it != dir->end(); 
       it++) {
    CDentry *dn = it->second;
    
    if (dn->is_null()) continue;

    CInode *in = dn->inode;
    if (!in) 
      continue;  // hmm, fixme!, what about REMOTE links?  
    
    dout(12) << "including inode " << *in << endl;

    // add this item
    // note: InodeStat makes note of whether inode data is readable.
    dnls.push_back( it->first );
    inls.push_back( new InodeStat(in, mds->get_nodeid()) );
    numfiles++;
  }
  return numfiles;
}


void Server::handle_client_readdir(MClientRequest *req,
				   CInode *diri)
{
  // it's a directory, right?
  if (!diri->is_dir()) {
    // not a dir
    dout(10) << "reply to " << *req << " readdir -ENOTDIR" << endl;
    reply_request(req, -ENOTDIR);
    return;
  }

  // which frag?
  frag_t fg = req->args.readdir.frag;

  // does it exist?
  if (diri->dirfragtree[fg] != fg) {
    dout(10) << "frag " << fg << " doesn't appear in fragtree " << diri->dirfragtree << endl;
    reply_request(req, -EAGAIN);
    return;
  }
  
  CDir *dir = try_open_auth_dir(diri, fg, req);
  if (!dir) return;

  // ok!
  assert(dir->is_auth());

  // check perm
  if (!mds->locker->inode_hard_read_start(diri,req))
    return;
  mds->locker->inode_hard_read_finish(diri);

  if (!dir->is_complete()) {
    // fetch
    dout(10) << " incomplete dir contents for readdir on " << *dir << ", fetching" << endl;
    dir->fetch(new C_MDS_RetryRequest(mds, req, diri));
    return;
  }

  // build dir contents
  list<InodeStat*> inls;
  list<string> dnls;
  int numfiles = encode_dir_contents(dir, inls, dnls);
  
  // . too
  dnls.push_back(".");
  inls.push_back(new InodeStat(diri, mds->get_nodeid()));
  ++numfiles;
  
  // yay, reply
  MClientReply *reply = new MClientReply(req);
  reply->take_dir_items(inls, dnls, numfiles);
  
  dout(10) << "reply to " << *req << " readdir " << numfiles << " files" << endl;
  reply->set_result(fg);
  
  //balancer->hit_dir(diri->dir);
  
  // reply
  reply_request(req, reply, diri);
}



// ------------------------------------------------

// MKNOD

class C_MDS_mknod_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CDentry *dn;
  CInode *newi;
  version_t pv;
public:
  C_MDS_mknod_finish(MDS *m, MClientRequest *r, CDentry *d, CInode *ni) :
    mds(m), req(r), dn(d), newi(ni),
    pv(d->get_projected_version()) {}
  void finish(int r) {
    assert(r == 0);

    // link the inode
    dn->get_dir()->link_inode(dn, newi);

    // dirty inode, dn, dir
    newi->mark_dirty(pv);

    // unlock
    mds->locker->dentry_xlock_finish(dn);

    // hit pop
    mds->balancer->hit_inode(newi, META_POP_IWR);

    // reply
    MClientReply *reply = new MClientReply(req, 0);
    reply->set_result(0);
    mds->server->reply_request(req, reply, newi);
  }
};

void Server::handle_client_mknod(MClientRequest *req, CInode *diri)
{
  CDir *dir = 0;
  CInode *newi = 0;
  CDentry *dn = 0;
  
  // make dentry and inode, xlock dentry.
  if (!prepare_mknod(req, diri, &dir, &newi, &dn)) 
    return;
  assert(dir);
  assert(newi);
  assert(dn);

  // it's a file.
  dn->pre_dirty();
  newi->inode.mode = req->args.mknod.mode;
  newi->inode.mode &= ~INODE_TYPE_MASK;
  newi->inode.mode |= INODE_MODE_FILE;
  
  // prepare finisher
  C_MDS_mknod_finish *fin = new C_MDS_mknod_finish(mds, req, dn, newi);
  EUpdate *le = new EUpdate("mknod");
  le->metablob.add_dir_context(dir);
  inode_t *pi = le->metablob.add_primary_dentry(dn, true, newi);
  pi->version = dn->get_projected_version();
  
  // log + wait
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}



/*
 * verify that the dir exists and would own the dname.
 * do not check if the dentry exists.
 */
CDir *Server::validate_dentry_dir(MClientRequest *req, CInode *diri, string& name)
{
  // make sure parent is a dir?
  if (!diri->is_dir()) {
    dout(7) << "validate_dentry_dir: not a dir" << endl;
    reply_request(req, -ENOTDIR);
    return false;
  }

  // which dirfrag?
  frag_t fg = diri->pick_dirfrag(name);

  CDir *dir = try_open_auth_dir(diri, fg, req);
  if (!dir)
    return 0;

  // dir auth pinnable?
  if (!dir->can_auth_pin()) {
    dout(7) << "validate_dentry_dir: dir " << *dir << " not pinnable, waiting" << endl;
    dir->add_waiter(CDir::WAIT_AUTHPINNABLE,
		    new C_MDS_RetryRequest(mds, req, diri));
    return false;
  }

  // frozen?
  if (dir->is_frozen()) {
    dout(7) << "dir is frozen " << *dir << endl;
    dir->add_waiter(CDir::WAIT_UNFREEZE,
                    new C_MDS_RetryRequest(mds, req, diri));
    return false;
  }

  return dir;
}

/*
 * prepare a mknod-type operation (mknod, mkdir, symlink, open+create).
 * create the inode and dentry, but do not link them.
 * pre_dirty the dentry+dir.
 * xlock the dentry.
 *
 * return val
 *  0 - wait for something
 *  1 - created
 *  2 - already exists (only if okexist=true)
 */
int Server::prepare_mknod(MClientRequest *req, CInode *diri, 
			  CDir **pdir, CInode **pin, CDentry **pdn, 
			  bool okexist) 
{
  dout(10) << "prepare_mknod " << req->get_filepath() << " in " << *diri << endl;
  
  // get containing directory (without last bit)
  filepath dirpath = req->get_filepath().prefixpath(req->get_filepath().depth() - 1);
  string name = req->get_filepath().last_dentry();
  
  CDir *dir = *pdir = validate_dentry_dir(req, diri, name);
  if (!dir) return 0;

  // make sure name doesn't already exist
  *pdn = dir->lookup(name);
  if (*pdn) {
    if (!(*pdn)->can_read(req)) {
      dout(10) << "waiting on (existing!) unreadable dentry " << **pdn << endl;
      dir->add_waiter(CDir::WAIT_DNREAD, name, new C_MDS_RetryRequest(mds, req, diri));
      return 0;
    }

    if (!(*pdn)->is_null()) {
      // name already exists
      if (okexist) {
        dout(10) << "dentry " << name << " exists in " << *dir << endl;
	*pin = (*pdn)->inode;
        return 2;
      } else {
        dout(10) << "dentry " << name << " exists in " << *dir << endl;
        reply_request(req, -EEXIST);
        return 0;
      }
    }
  }

  // make sure dir is complete
  if (!dir->is_complete()) {
    dout(7) << " incomplete dir contents for " << *dir << ", fetching" << endl;
    dir->fetch(new C_MDS_RetryRequest(mds, req, diri));
    return 0;
  }

  // create null dentry
  if (!*pdn) 
    *pdn = dir->add_dentry(name, 0);

  // xlock dentry
  bool res = mds->locker->dentry_xlock_start(*pdn, req, diri);
  if (!res) 
    return 0;
  
  // yay!

  // create inode?
  if (pin) {
    *pin = mdcache->create_inode();
    (*pin)->inode.uid = req->get_caller_uid();
    (*pin)->inode.gid = req->get_caller_gid();
    (*pin)->inode.ctime = (*pin)->inode.mtime = (*pin)->inode.atime = g_clock.gettime();   // now
    // note: inode.version will get set by finisher's mark_dirty.
  }

  // bump modify pop
  mds->balancer->hit_dir(dir, META_POP_DWR);

  return 1;
}





// MKDIR

void Server::handle_client_mkdir(MClientRequest *req, CInode *diri)
{
  CDir *dir = 0;
  CInode *newi = 0;
  CDentry *dn = 0;
  
  // make dentry and inode, xlock dentry.
  if (!prepare_mknod(req, diri, &dir, &newi, &dn)) 
    return;
  assert(dir);
  assert(newi);
  assert(dn);

  // it's a directory.
  dn->pre_dirty();
  newi->inode.mode = req->args.mkdir.mode;
  newi->inode.mode &= ~INODE_TYPE_MASK;
  newi->inode.mode |= INODE_MODE_DIR;
  newi->inode.layout = g_OSD_MDDirLayout;

  // ...and that new dir is empty.
  CDir *newdir = newi->get_or_open_dirfrag(mds->mdcache, frag_t());
  newdir->mark_complete();
  newdir->mark_dirty(newdir->pre_dirty());

  // prepare finisher
  C_MDS_mknod_finish *fin = new C_MDS_mknod_finish(mds, req, dn, newi);
  EUpdate *le = new EUpdate("mkdir");
  le->metablob.add_dir_context(dir);
  inode_t *pi = le->metablob.add_primary_dentry(dn, true, newi);
  pi->version = dn->get_projected_version();
  le->metablob.add_dir(newdir, true);
  
  // log + wait
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);


  /* old export heuristic.  pbly need to reimplement this at some point.    
  if (
      diri->dir->is_auth() &&
      diri->dir->is_rep() &&
      newdir->is_auth() &&
      !newdir->is_hashing()) {
    int dest = rand() % mds->mdsmap->get_num_mds();
    if (dest != whoami) {
      dout(10) << "exporting new dir " << *newdir << " in replicated parent " << *diri->dir << endl;
      mdcache->migrator->export_dir(newdir, dest);
    }
  }
  */
}



// SYMLINK

void Server::handle_client_symlink(MClientRequest *req, CInode *diri)
{
  CDir *dir = 0;
  CInode *newi = 0;
  CDentry *dn = 0;

  // make dentry and inode, xlock dentry.
  if (!prepare_mknod(req, diri, &dir, &newi, &dn)) 
    return;
  assert(dir);
  assert(newi);
  assert(dn);

  // it's a symlink
  dn->pre_dirty();
  newi->inode.mode &= ~INODE_TYPE_MASK;
  newi->inode.mode |= INODE_MODE_SYMLINK;
  newi->symlink = req->get_sarg();

  // prepare finisher
  C_MDS_mknod_finish *fin = new C_MDS_mknod_finish(mds, req, dn, newi);
  EUpdate *le = new EUpdate("symlink");
  le->metablob.add_dir_context(dir);
  inode_t *pi = le->metablob.add_primary_dentry(dn, true, newi);
  pi->version = dn->get_projected_version();
  
  // log + wait
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}





// LINK

class C_MDS_LinkTraverse : public Context {
  Server *server;
  MClientRequest *req;
  CInode *ref;
public:
  vector<CDentry*> trace;
  C_MDS_LinkTraverse(Server *server, MClientRequest *req, CInode *ref) {
    this->server = server;
    this->req = req;
    this->ref = ref;
  }
  void finish(int r) {
    server->handle_client_link_2(r, req, ref, trace);
  }
};

void Server::handle_client_link(MClientRequest *req, CInode *ref)
{
  // figure out name
  string dname = req->get_filepath().last_dentry();
  dout(7) << "handle_client_link dname is " << dname << endl;
  
  // validate dir
  CDir *dir = validate_dentry_dir(req, ref, dname);
  if (!dir) return;

  // discover link target
  filepath target = req->get_sarg();
  dout(7) << "handle_client_link discovering target " << target << endl;
  C_MDS_LinkTraverse *onfinish = new C_MDS_LinkTraverse(this, req, ref);
  Context *ondelay = new C_MDS_RetryRequest(mds, req, ref);
  
  mdcache->path_traverse(target, onfinish->trace, false,
                         req, ondelay,
                         MDS_TRAVERSE_DISCOVER,  //XLOCK, 
                         onfinish);
}


void Server::handle_client_link_2(int r, MClientRequest *req, CInode *diri, vector<CDentry*>& trace)
{
  // target dne?
  if (r < 0) {
    dout(7) << "target " << req->get_sarg() << " dne" << endl;
    reply_request(req, r);
    return;
  }
  assert(r == 0);

  // identify target inode
  CInode *targeti = mdcache->get_root();
  if (trace.size()) targeti = trace[trace.size()-1]->inode;
  assert(targeti);

  // dir?
  dout(7) << "target is " << *targeti << endl;
  if (targeti->is_dir()) {
    dout(7) << "target is a dir, failing" << endl;
    reply_request(req, -EINVAL);
    return;
  }

  // can we create the dentry?
  CDir *dir = 0;
  CDentry *dn = 0;
  
  // make dentry and inode, xlock dentry.
  r = prepare_mknod(req, diri, &dir, 0, &dn);
  if (!r) 
    return; // wait on something
  assert(dir);
  assert(dn);

  // ok!
  assert(dn->is_xlockedbyme(req));

  // local or remote?
  if (targeti->is_auth()) 
    link_local(req, diri, dn, targeti);
  else 
    link_remote(req, diri, dn, targeti);
}


class C_MDS_link_local_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CDentry *dn;
  CInode *targeti;
  version_t dpv;
  time_t tctime;
  time_t tpv;
public:
  C_MDS_link_local_finish(MDS *m, MClientRequest *r, CDentry *d, CInode *ti, time_t ct) :
    mds(m), req(r), dn(d), targeti(ti),
    dpv(d->get_projected_version()),
    tctime(ct), 
    tpv(targeti->get_parent_dn()->get_projected_version()) {}
  void finish(int r) {
    assert(r == 0);
    mds->server->_link_local_finish(req, dn, targeti, dpv, tctime, tpv);
  }
};


void Server::link_local(MClientRequest *req, CInode *diri,
			CDentry *dn, CInode *targeti)
{
  dout(10) << "link_local " << *dn << " to " << *targeti << endl;

  // anchor target?
  if (targeti->get_parent_dir() == dn->get_dir()) {
    dout(7) << "target is in the same dir, sweet" << endl;
  } 
  else if (targeti->is_anchored() && !targeti->is_unanchoring()) {
    dout(7) << "target anchored already (nlink=" << targeti->inode.nlink << "), sweet" << endl;
  } else {
    dout(7) << "target needs anchor, nlink=" << targeti->inode.nlink << ", creating anchor" << endl;
    
    mdcache->anchor_create(targeti,
			   new C_MDS_RetryRequest(mds, req, diri));
    return;
  }

  // wrlock the target inode
  if (!mds->locker->inode_hard_write_start(targeti, req))
    return;  // fw or (wait for) lock

  // ok, let's do it.
  // prepare log entry
  EUpdate *le = new EUpdate("link_local");

  // predirty
  dn->pre_dirty();
  version_t tpdv = targeti->pre_dirty();
  
  // add to event
  le->metablob.add_dir_context(dn->get_dir());
  le->metablob.add_remote_dentry(dn, true, targeti->ino());  // new remote
  le->metablob.add_dir_context(targeti->get_parent_dir());
  inode_t *pi = le->metablob.add_primary_dentry(targeti->parent, true, targeti);  // update old primary

  // update journaled target inode
  pi->nlink++;
  pi->ctime = g_clock.gettime();
  pi->version = tpdv;

  // finisher
  C_MDS_link_local_finish *fin = new C_MDS_link_local_finish(mds, req, dn, targeti, pi->ctime);
  
  // log + wait
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}

void Server::_link_local_finish(MClientRequest *req, CDentry *dn, CInode *targeti,
				version_t dpv, time_t tctime, version_t tpv)
{
  dout(10) << "_link_local_finish " << *dn << " to " << *targeti << endl;

  // link and unlock the new dentry
  dn->set_remote_ino(targeti->ino());
  dn->set_version(dpv);
  dn->mark_dirty(dpv);

  // update the target
  targeti->inode.nlink++;
  targeti->inode.ctime = tctime;
  targeti->mark_dirty(tpv);

  // unlock the new dentry and target inode
  mds->locker->dentry_xlock_finish(dn);
  mds->locker->inode_hard_write_finish(targeti);

  // bump target popularity
  mds->balancer->hit_inode(targeti, META_POP_IWR);

  // reply
  MClientReply *reply = new MClientReply(req, 0);
  reply_request(req, reply, dn->get_dir()->get_inode());  // FIXME: imprecise ref
}



void Server::link_remote(MClientRequest *req, CInode *ref,
			 CDentry *dn, CInode *targeti)
{
  dout(10) << "link_remote " << *dn << " to " << *targeti << endl;

  // pin the target replica in our cache
  assert(!targeti->is_auth());
  mdcache->request_pin_inode(req, targeti);

  // 1. send LinkPrepare to dest (lock target on dest, journal target update)




  // 2. create+journal new dentry, as with link_local.
  // 3. send LinkCommit to dest (unlocks target on dest, journals commit)  

  // IMPLEMENT ME
  MClientReply *reply = new MClientReply(req, -EXDEV);
  reply_request(req, reply, dn->get_dir()->get_inode());
}


/*
void Server::handle_client_link_finish(MClientRequest *req, CInode *ref,
				       CDentry *dn, CInode *targeti)
{
  // create remote link
  dn->dir->link_inode(dn, targeti->ino());
  dn->link_remote( targeti );   // since we have it
  dn->_mark_dirty(); // fixme
  
  mds->balancer->hit_dir(dn->dir, META_POP_DWR);

  // done!
  commit_request(req, new MClientReply(req, 0), ref,
                 0);          // FIXME i should log something
}
*/

/*
class C_MDS_RemoteLink : public Context {
  Server *server;
  MClientRequest *req;
  CInode *ref;
  CDentry *dn;
  CInode *targeti;
public:
  C_MDS_RemoteLink(Server *server, MClientRequest *req, CInode *ref, CDentry *dn, CInode *targeti) {
    this->server = server;
    this->req = req;
    this->ref = ref;
    this->dn = dn;
    this->targeti = targeti;
  }
  void finish(int r) {
    if (r > 0) { // success
      // yay
      server->handle_client_link_finish(req, ref, dn, targeti);
    } 
    else if (r == 0) {
      // huh?  retry!
      assert(0);
      server->dispatch_request(req, ref);      
    } else {
      // link failed
      server->reply_request(req, r);
    }
  }
};


  } else {
    // remote: send nlink++ request, wait
    dout(7) << "target is remote, sending InodeLink" << endl;
    mds->send_message_mds(new MInodeLink(targeti->ino(), mds->get_nodeid()), targeti->authority().first, MDS_PORT_CACHE);
    
    // wait
    targeti->add_waiter(CInode::WAIT_LINK,
                        new C_MDS_RemoteLink(this, req, diri, dn, targeti));
    return;
  }

*/





// UNLINK

void Server::handle_client_unlink(MClientRequest *req, 
				  CInode *diri)
{
  // rmdir or unlink?
  bool rmdir = false;
  if (req->get_op() == MDS_OP_RMDIR) rmdir = true;
 
  // find it
  if (req->get_filepath().depth() == 0) {
    dout(7) << "can't rmdir root" << endl;
    reply_request(req, -EINVAL);
    return;
  }
  string name = req->get_filepath().last_dentry();

  // make sure parent is a dir?
  if (!diri->is_dir()) {
    dout(7) << "parent not a dir " << *diri << endl;
    reply_request(req, -ENOTDIR);
    return;
  }

  // get the dir, if it's not frozen etc.
  CDir *dir = validate_dentry_dir(req, diri, name);
  if (!dir) return;
  // ok, it's auth, and authpinnable.

  // does the dentry exist?
  CDentry *dn = dir->lookup(name);
  if (!dn) {
    if (!dir->is_complete()) {
      dout(7) << "handle_client_rmdir/unlink missing dn " << name
	      << " but dir not complete, fetching " << *dir << endl;
      dir->fetch(new C_MDS_RetryRequest(mds, req, diri));
    } else {
      dout(7) << "handle_client_rmdir/unlink dne " << name << " in " << *dir << endl;
      reply_request(req, -ENOENT);
    }
    return;
  }

  if (rmdir) {
    dout(7) << "handle_client_rmdir on " << *dn << endl;
  } else {
    dout(7) << "handle_client_unlink on " << *dn << endl;
  }

  // have it.  locked?
  if (!dn->can_read(req)) {
    dout(10) << " waiting on " << *dn << endl;
    dir->add_waiter(CDir::WAIT_DNREAD, name,
                    new C_MDS_RetryRequest(mds, req, diri));
    return;
  }

  // null?
  if (dn->is_null()) {
    dout(10) << "unlink on null dn " << *dn << endl;
    reply_request(req, -ENOENT);
    return;
  }
  // dn looks ok.

  // remote?  if so, open up the inode.
  if (!dn->inode) {
    assert(dn->is_remote());
    CInode *in = mdcache->get_inode(dn->get_remote_ino());
    if (in) {
      dout(7) << "linking in remote in " << *in << endl;
      dn->link_remote(in);
    } else {
      dout(10) << "remote dn, opening inode for " << *dn << endl;
      mdcache->open_remote_ino(dn->get_remote_ino(), req, 
			       new C_MDS_RetryRequest(mds, req, diri));
      return;
    }
  }
  assert(dn->inode);

  // ok!
  CInode *in = dn->inode;

  // rmdir vs is_dir 
  if (in->is_dir()) {
    if (rmdir) {
      // do empty directory checks
      if (!_verify_rmdir(req, diri, in))
	return;
    } else {
      dout(7) << "handle_client_unlink on dir " << *in << ", returning error" << endl;
      reply_request(req, -EISDIR);
      return;
    }
  } else {
    if (rmdir) {
      // unlink
      dout(7) << "handle_client_rmdir on non-dir " << *in << ", returning error" << endl;
      reply_request(req, -ENOTDIR);
      return;
    }
  }

  dout(7) << "handle_client_unlink/rmdir on " << *in << endl;

  // treat this like a rename?
  if (dn->is_primary() &&        // primary link, and
      (in->inode.nlink > 1 ||    // there are other hard links, or
       in->get_caps_wanted())) { // file is open (FIXME need better condition here)
    // treat as a rename into the dangledir.

    // IMPLEMENT ME **** FIXME ****
    MClientReply *reply = new MClientReply(req, -EXDEV);
    reply_request(req, reply, dn->get_dir()->get_inode());
    return;
  }
  
  // xlock dentry
  if (!mds->locker->dentry_xlock_start(dn, req, diri))
    return;

  mds->balancer->hit_dir(dn->dir, META_POP_DWR);

  // ok!
  if (dn->is_remote() && !dn->inode->is_auth()) 
    _unlink_remote(req, dn, in);
  else
    _unlink_local(req, dn, in);
}



class C_MDS_unlink_local_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CDentry *dn;
  CInode *in;
  version_t ipv;
  time_t ictime;
  version_t dpv;
public:
  C_MDS_unlink_local_finish(MDS *m, MClientRequest *r, CDentry *d, CInode *i,
			    version_t v, time_t ct) :
    mds(m), req(r), dn(d), in(i),
    ipv(v), ictime(ct),
    dpv(d->get_projected_version()) { }
  void finish(int r) {
    assert(r == 0);
    mds->server->_unlink_local_finish(req, dn, in, ipv, ictime, dpv);
  }
};


void Server::_unlink_local(MClientRequest *req, CDentry *dn, CInode *in)
{
  dout(10) << "_unlink_local " << *dn << endl;
  
  // if we're not the only link, wrlock the target (we need to nlink--)
  if (in->inode.nlink > 1) {
    assert(dn->is_remote());  // unlinking primary is handled like a rename.. not here

    dout(10) << "_unlink_local nlink>1, wrlocking " << *in << endl;
    if (!mds->locker->inode_hard_write_start(in, req))
      return;  // fw or (wait for) lock
  }
  
  // ok, let's do it.
  // prepare log entry
  EUpdate *le = new EUpdate("unlink_local");

  // predirty
  version_t ipv = in->pre_dirty();
  if (dn->is_remote()) 
    dn->pre_dirty();  // predirty dentry too
  
  // the unlinked dentry
  le->metablob.add_dir_context(dn->get_dir());
  le->metablob.add_null_dentry(dn, true);

  // remote inode nlink--?
  inode_t *pi = 0;
  if (dn->is_remote()) {
    le->metablob.add_dir_context(in->get_parent_dir());
    pi = le->metablob.add_primary_dentry(in->parent, true, in);  // update primary
    
    // update journaled target inode
    pi->nlink--;
    pi->ctime = g_clock.gettime();
    pi->version = ipv;
  } else {
    le->metablob.add_destroyed_inode(in->inode);
  }

  // finisher
  C_MDS_unlink_local_finish *fin = new C_MDS_unlink_local_finish(mds, req, dn, in, 
								 ipv, pi ? pi->ctime:0);
  
  // log + wait
  mdlog->submit_entry(le);
  mdlog->wait_for_sync(fin);
}

void Server::_unlink_local_finish(MClientRequest *req, 
				  CDentry *dn, CInode *in,
				  version_t ipv, time_t ictime, version_t dpv) 
{
  dout(10) << "_unlink_local " << *dn << endl;

  // update remote inode?
  if (dn->is_remote()) {
    assert(ipv);
    assert(ictime);
    in->inode.ctime = ictime;
    in->inode.nlink--;
    in->mark_dirty(ipv);

    // unlock inode (and share nlink news w/ replicas)
    mds->locker->inode_hard_write_finish(in);
  }

  // unlink inode (dn now null)
  CDir *dir = dn->dir;
  dn->mark_dirty(dpv);
  dir->unlink_inode(dn);
  
  // share unlink news with replicas
  for (map<int,int>::iterator it = dn->replicas_begin();
       it != dn->replicas_end();
       it++) {
    dout(7) << "_unlink_local_finish sending MDentryUnlink to mds" << it->first << endl;
    mds->send_message_mds(new MDentryUnlink(dir->dirfrag(), dn->name), it->first, MDS_PORT_CACHE);
  }

  // unlock (now null) dn
  mds->locker->dentry_xlock_finish(dn);
  
  // purge+remove inode?
  if (in->inode.nlink == 0) {
    mdcache->purge_inode(&in->inode);
    mdcache->remove_inode(in);
  }
  
  // bump target popularity
  mds->balancer->hit_dir(dir, META_POP_DWR);

  // reply
  MClientReply *reply = new MClientReply(req, 0);
  reply_request(req, reply, dir->get_inode());  // FIXME: imprecise ref
}



void Server::_unlink_remote(MClientRequest *req, CDentry *dn, CInode *in) 
{


  // IMPLEMENT ME
  MClientReply *reply = new MClientReply(req, -EXDEV);
  reply_request(req, reply, dn->get_dir()->get_inode());
}




/** _verify_rmdir
 *
 * verify that a directory is empty (i.e. we can rmdir it),
 * and make sure it is part of the same subtree (i.e. local)
 * so that rmdir will occur locally.
 *
 * @param in is the inode being rmdir'd.
 */
bool Server::_verify_rmdir(MClientRequest *req, CInode *ref, CInode *in)
{
  dout(10) << "_verify_rmdir " << *in << endl;
  assert(in->is_auth());

  list<frag_t> frags;
  in->dirfragtree.get_leaves(frags);

  for (list<frag_t>::iterator p = frags.begin();
       p != frags.end();
       ++p) {
    CDir *dir = in->get_dirfrag(*p);
    if (!dir) 
      dir = in->get_or_open_dirfrag(mdcache, *p);
    assert(dir);

    // dir looks empty but incomplete?
    if (dir->is_auth() &&
	dir->get_size() == 0 && 
	!dir->is_complete()) {
      dout(7) << "_verify_rmdir fetching incomplete dir " << *dir << endl;
      dir->fetch(new C_MDS_RetryRequest(mds, req, ref));
      return false;
    }
    
    // does the frag _look_ empty?
    if (dir->get_size()) {
      dout(10) << "_verify_rmdir nonauth bit has " << dir->get_size() << " items, not empty " << *dir << endl;
      reply_request(req, -ENOTEMPTY);
      return false;
    }
    
    // not dir auth?
    if (!dir->is_auth()) {
      // hmm. we need it to import.  how to make that happen?
      // and wait on it?
      assert(0);  // IMPLEMENT ME
    }
  }

  return true;
}
/*
      // export sanity check
      if (!in->is_auth()) {
        // i should be exporting this now/soon, since the dir is empty.
        dout(7) << "handle_client_rmdir dir is auth, but not inode." << endl;
	mdcache->migrator->export_empty_import(in->dir);          
        in->dir->add_waiter(CDir::WAIT_UNFREEZE,
                            new C_MDS_RetryRequest(mds, req, diri));
        return;
      }
*/





// RENAME

class C_MDS_RenameTraverseDst : public Context {
  Server *server;
  MClientRequest *req;
  CInode *ref;
  CInode *srcdiri;
  CDir *srcdir;
  CDentry *srcdn;
  filepath destpath;
public:
  vector<CDentry*> trace;
  
  C_MDS_RenameTraverseDst(Server *server,
                          MClientRequest *req, 
                          CInode *ref,
                          CInode *srcdiri,
                          CDir *srcdir,
                          CDentry *srcdn,
                          filepath& destpath) {
    this->server = server;
    this->req = req;
    this->ref = ref;
    this->srcdiri = srcdiri;
    this->srcdir = srcdir;
    this->srcdn = srcdn;
    this->destpath = destpath;
  }
  void finish(int r) {
    server->handle_client_rename_2(req, ref,
				   srcdiri, srcdir, srcdn, destpath,
				   trace, r);
  }
};


/*
  
  weirdness iwith rename:
    - ref inode is what was originally srcdiri, but that may change by the tiem
      the rename actually happens.  for all practical purpose, ref is useless except
      for C_MDS_RetryRequest

 */
void Server::handle_client_rename(MClientRequest *req,
                               CInode *ref)
{
  dout(7) << "handle_client_rename on " << *req << endl;

  // sanity checks
  if (req->get_filepath().depth() == 0) {
    dout(7) << "can't rename root" << endl;
    reply_request(req, -EINVAL);
    return;
  }
  // mv a/b a/b/c  -- meaningless
  if (req->get_sarg().compare( 0, req->get_path().length(), req->get_path()) == 0 &&
      req->get_sarg().c_str()[ req->get_path().length() ] == '/') {
    dout(7) << "can't rename to underneath myself" << endl;
    reply_request(req, -EINVAL);
    return;
  }

  // mv blah blah  -- also meaningless
  if (req->get_sarg() == req->get_path()) {
    dout(7) << "can't rename something to itself (or into itself)" << endl;
    reply_request(req, -EINVAL);
    return;
  }
  
  // traverse to source
  /*
    this is abnoraml, just for rename.  since we don't pin source path 
    (because we don't want to screw up the lock ordering) the ref inode 
    (normally/initially srcdiri) may move, and this may fail.
 -> so, re-traverse path.  and make sure we request_finish in the case of a forward!
   */
  filepath refpath = req->get_filepath();
  string srcname = refpath.last_dentry();
  refpath = refpath.prefixpath(refpath.depth()-1);

  dout(7) << "handle_client_rename src traversing to srcdir " << refpath << endl;
  vector<CDentry*> trace;
  int r = mdcache->path_traverse(refpath, trace, true,
                                 req, new C_MDS_RetryRequest(mds, req, ref),
                                 MDS_TRAVERSE_FORWARD);
  if (r == 2) {
    dout(7) << "path traverse forwarded, ending request, doing manual request_cleanup" << endl;
    dout(7) << "(pseudo) request_forward to 9999 req " << *req << endl;
    mdcache->request_cleanup(req);  // not _finish (deletes) or _forward (path_traverse did that)
    return;
  }
  if (r > 0) return;
  if (r < 0) {   // dne or something.  got renamed out from under us, probably!
    dout(7) << "traverse r=" << r << endl;
    reply_request(req, r);
    return;
  }
  
  CInode *srcdiri;
  if (trace.size()) 
    srcdiri = trace[trace.size()-1]->inode;
  else
    srcdiri = mdcache->get_root();

  dout(7) << "handle_client_rename srcdiri is " << *srcdiri << endl;

  dout(7) << "handle_client_rename srcname is " << srcname << endl;

  // make sure parent is a dir?
  if (!srcdiri->is_dir()) {
    dout(7) << "srcdiri not a dir " << *srcdiri << endl;
    reply_request(req, -EINVAL);
    return;
  }

  frag_t srcfg = srcdiri->pick_dirfrag(srcname);

  // am i not open, not auth?
  if (!srcdiri->get_dirfrag(srcfg) && !srcdiri->is_auth()) {
    int dirauth = srcdiri->authority().first;
    dout(7) << "don't know dir auth, not open, srcdir auth is probably " << dirauth << endl;
    mdcache->request_forward(req, dirauth);
    return;
  }

  CDir *srcdir = try_open_auth_dir(srcdiri, srcfg, req);
  if (!srcdir) return;
  dout(7) << "handle_client_rename srcdir is " << *srcdir << endl;
  
  // ok, done passing buck.

  // src dentry
  CDentry *srcdn = srcdir->lookup(srcname);

  // xlocked?
  if (srcdn && !srcdn->can_read(req)) {
    dout(10) << " waiting on " << *srcdn << endl;
    srcdir->add_waiter(CDir::WAIT_DNREAD,
                       srcname,
                       new C_MDS_RetryRequest(mds, req, srcdiri));
    return;
  }
  
  if ((srcdn && !srcdn->inode) ||
      (!srcdn && srcdir->is_complete())) {
    dout(10) << "handle_client_rename src dne " << endl;
    reply_request(req, -EEXIST);
    return;
  }
  
  if (!srcdn && !srcdir->is_complete()) {
    dout(10) << "readding incomplete dir" << endl;
    srcdir->fetch(new C_MDS_RetryRequest(mds, req, srcdiri));
    return;
  }
  assert(srcdn && srcdn->inode);


  dout(10) << "handle_client_rename srcdn is " << *srcdn << endl;
  dout(10) << "handle_client_rename srci is " << *srcdn->inode << endl;

  // pin src in cache (so it won't expire)
  mdcache->request_pin_inode(req, srcdn->inode);
  
  // find the destination, normalize
  // discover, etc. on the way... just get it on the local node.
  filepath destpath = req->get_sarg();   

  C_MDS_RenameTraverseDst *onfinish = new C_MDS_RenameTraverseDst(this, req, ref, srcdiri, srcdir, srcdn, destpath);
  Context *ondelay = new C_MDS_RetryRequest(mds, req, ref);
  
  /*
   * use DISCOVERXLOCK mode:
   *   the dest may not exist, and may be xlocked from a remote host
   *   we want to succeed if we find the xlocked dentry
   * ??
   */
  mdcache->path_traverse(destpath, onfinish->trace, false,
                         req, ondelay,
                         MDS_TRAVERSE_DISCOVER,  //XLOCK, 
                         onfinish);
}

void Server::handle_client_rename_2(MClientRequest *req,
                                 CInode *ref,
                                 CInode *srcdiri,
                                 CDir *srcdir,
                                 CDentry *srcdn,
                                 filepath& destpath,
                                 vector<CDentry*>& trace,
                                 int r)
{
  dout(7) << "handle_client_rename_2 on " << *req << endl;
  dout(12) << " r = " << r << " trace depth " << trace.size() << "  destpath depth " << destpath.depth() << endl;

  CInode *srci = srcdn->inode;
  assert(srci);
  CDir*  destdir = 0;
  string destname;
  
  // what is the dest?  (dir or file or complete filename)
  // note: trace includes root, destpath doesn't (include leading /)
  if (trace.size() && trace[trace.size()-1]->inode == 0) {
    dout(10) << "dropping null dentry from tail of trace" << endl;
    trace.pop_back();    // drop it!
  }
  
  CInode *d;
  if (trace.size()) 
    d = trace[trace.size()-1]->inode;
  else
    d = mdcache->get_root();
  assert(d);
  dout(10) << "handle_client_rename_2 traced to " << *d << ", trace size = " << trace.size() << ", destpath = " << destpath.depth() << endl;
  
  // make sure i can open the dir?
  if (d->is_dir() && !d->dir_is_auth() && !d->dir) {
    // discover it
    mdcache->open_remote_dir(d, frag_t(),  // FIXME
                             new C_MDS_RetryRequest(mds, req, ref));
    return;
  }

  frag_t dfg = d->pick_dirfrag(destname);
  if (trace.size() == destpath.depth()) {
    if (d->is_dir()) {
      // mv /some/thing /to/some/dir 
      destdir = try_open_dir(d, dfg, req);        // /to/some/dir
      if (!destdir) return;
      destname = req->get_filepath().last_dentry();  // thing
      destpath.push_dentry(destname);
    } else {
      // mv /some/thing /to/some/existing_filename
      destdir = trace[trace.size()-1]->dir;       // /to/some
      destname = destpath.last_dentry();             // existing_filename
    }
  }
  else if (trace.size() == destpath.depth()-1) {
    if (d->is_dir()) {
      // mv /some/thing /to/some/place_that_maybe_dne     (we might be replica)
      destdir = try_open_dir(d, dfg, req); // /to/some
      if (!destdir) return;
      destname = destpath.last_dentry();    // place_that_MAYBE_dne
    } else {
      dout(7) << "dest dne" << endl;
      reply_request(req, -EINVAL);
      return;
    }
  }
  else {
    assert(trace.size() < destpath.depth()-1);
    // check traverse return value
    if (r > 0) {
      return;  // discover, readdir, etc.
    }

    // ??
    assert(r < 0 || trace.size() == 0);  // musta been an error

    // error out
    dout(7) << " rename dest " << destpath << " dne" << endl;
    reply_request(req, -EINVAL);
    return;
  }

  string srcpath = req->get_path();
  dout(10) << "handle_client_rename_2 srcpath " << srcpath << endl;
  dout(10) << "handle_client_rename_2 destpath " << destpath << endl;

  // src == dest?
  if (srcdn->get_dir() == destdir && srcdn->name == destname) {
    dout(7) << "rename src=dest, same file " << endl;
    reply_request(req, -EINVAL);
    return;
  }

  // does destination exist?  (is this an overwrite?)
  CDentry *destdn = destdir->lookup(destname);
  CInode  *oldin = 0;
  if (destdn) {
    oldin = destdn->get_inode();
    
    if (oldin) {
      // make sure it's also a file!
      // this can happen, e.g. "mv /some/thing /a/dir" where /a/dir/thing exists and is a dir.
      if (oldin->is_dir()) {
        // fail!
        dout(7) << "dest exists and is dir" << endl;
        reply_request(req, -EISDIR);
        return;
      }

      if (srcdn->inode->is_dir() &&
          !oldin->is_dir()) {
        dout(7) << "cannot overwrite non-directory with directory" << endl;
        reply_request(req, -EISDIR);
        return;
      }
    }

    dout(7) << "dest exists " << *destdn << endl;
    if (destdn->get_inode()) {
      dout(7) << "destino is " << *destdn->get_inode() << endl;
    } else {
      dout(7) << "dest dn is a NULL stub" << endl;
    }
  } else {
    dout(7) << "dest dn dne (yet)" << endl;
  }
  

  // local or remote?
  int srcauth = srcdir->dentry_authority(srcdn->name).first;
  int destauth = destdir->dentry_authority(destname).first;
  dout(7) << "handle_client_rename_2 destname " << destname << " destdir " << *destdir << " auth " << destauth << endl;
  
  // 
  if (srcauth != mds->get_nodeid() || 
      destauth != mds->get_nodeid()) {
    dout(7) << "rename has remote dest " << destauth << endl;
    dout(7) << "FOREIGN RENAME" << endl;
    
    // punt?
    if (false && srcdn->inode->is_dir()) {
      reply_request(req, -EINVAL);  
      return; 
    }

  } else {
    dout(7) << "rename is local" << endl;
  }

  handle_client_rename_local(req, ref,
                             srcpath, srcdiri, srcdn, 
                             destpath.get_path(), destdir, destdn, destname);
  return;
}




void Server::handle_client_rename_local(MClientRequest *req,
					CInode *ref,
					const string& srcpath,
					CInode *srcdiri,
					CDentry *srcdn,
					const string& destpath,
					CDir *destdir,
					CDentry *destdn,
					const string& destname)
{
  //bool everybody = false;
  //if (true || srcdn->inode->is_dir()) {
    /* overkill warning: lock w/ everyone for simplicity.  FIXME someday!  along with the foreign rename crap!
       i could limit this to cases where something beneath me is exported.
       could possibly limit the list.    (maybe.)
       Underlying constraint is that, regardless of the order i do the xlocks, and whatever
       imports/exports might happen in the process, the destdir _must_ exist on any node
       importing something beneath me when rename finishes, or else mayhem ensues when
       their import is dangling in the cache.
     */
    /*
      having made a proper mess of this on the first pass, here is my plan:
      
      - xlocks of src, dest are done in lex order
      - xlock is optional.. if you have the dentry, lock it, if not, don't.
      - if you discover an xlocked dentry, you get the xlock.

      possible trouble:
      - you have an import beneath the source, and don't have the dest dir.
        - when the actual rename happens, you discover the dest
        - actually, do this on any open dir, so we don't detach whole swaths
          of our cache.
      
      notes:
      - xlocks are initiated from authority, as are discover_replies, so replicas are 
        guaranteed to either not have dentry, or to have it xlocked. 
      - 
      - foreign xlocks are eventually unraveled by the initiator on success or failure.

      todo to make this work:
      - hose bool everybody param crap
      /- make handle_lock_dn not discover, clean up cases
      /- put dest path in MRenameNotify
      /- make rename_notify discover if its a dir
      /  - this will catch nested imports too, obviously
      /- notify goes to merged list on local rename
      /- notify goes to everybody on a foreign rename 
      /- handle_notify needs to gracefully ignore spurious notifies
    */
  //dout(7) << "handle_client_rename_local: overkill?  doing xlocks with _all_ nodes" << endl;
  //everybody = true;
  //}

  bool srclocal = srcdn->dir->dentry_authority(srcdn->name).first == mds->get_nodeid();
  bool destlocal = destdir->dentry_authority(destname).first == mds->get_nodeid();

  dout(7) << "handle_client_rename_local: src local=" << srclocal << " " << *srcdn << endl;
  if (destdn) {
    dout(7) << "handle_client_rename_local: dest local=" << destlocal << " " << *destdn << endl;
  } else {
    dout(7) << "handle_client_rename_local: dest local=" << destlocal << " dn dne yet" << endl;
  }

  /* lock source and dest dentries, in lexicographic order.
   */
  bool dosrc = srcpath < destpath;
  for (int i=0; i<2; i++) {
    if (dosrc) {

      // src
      if (srclocal) {
        if (!srcdn->is_xlockedbyme(req) &&
            !mds->locker->dentry_xlock_start(srcdn, req, ref))
          return;  
      } else {
        if (!srcdn || srcdn->xlockedby != req) {
          mds->locker->dentry_xlock_request(srcdn->dir, srcdn->name, false, req, new C_MDS_RetryRequest(mds, req, ref));
          return;
        }
      }
      dout(7) << "handle_client_rename_local: srcdn is xlock " << *srcdn << endl;
      
    } else {

      if (destlocal) {
        // dest
        if (!destdn) destdn = destdir->add_dentry(destname);
        if (!destdn->is_xlockedbyme(req) &&
            !mds->locker->dentry_xlock_start(destdn, req, ref)) {
          if (destdn->is_clean() && destdn->is_null() && destdn->is_sync()) destdir->remove_dentry(destdn);
          return;
        }
      } else {
        if (!destdn || destdn->xlockedby != req) {
          /* NOTE: require that my xlocked item be a leaf/file, NOT a dir.  in case
           * my traverse and determination of dest vs dest/srcfilename was out of date.
           */
          mds->locker->dentry_xlock_request(destdir, destname, true, req, new C_MDS_RetryRequest(mds, req, ref));
          return;
        }
      }
      dout(7) << "handle_client_rename_local: destdn is xlock " << *destdn << endl;

    }
    
    dosrc = !dosrc;
  }

  
  // final check: verify if dest exists that src is a file

  // FIXME: is this necessary?

  if (destdn->inode) {
    if (destdn->inode->is_dir()) {
      dout(7) << "handle_client_rename_local failing, dest exists and is a dir: " << *destdn->inode << endl;
      assert(0);
      reply_request(req, -EINVAL);  
      return; 
    }
    if (srcdn->inode->is_dir()) {
      dout(7) << "handle_client_rename_local failing, dest exists and src is a dir: " << *destdn->inode << endl;
      assert(0);
      reply_request(req, -EINVAL);  
      return; 
    }
  } else {
    // if destdn->inode is null, then we know it's a non-existent dest,
    // why?  because if it's local, it dne.  and if it's remote, we xlocked with 
    // REQXLOCKC, which will only allow you to lock a file.
    // so we know dest is a file, or non-existent
    if (!destlocal) {
      if (srcdn->inode->is_dir()) { 
        // help: maybe the dest exists and is a file?   ..... FIXME
      } else {
        // we're fine, src is file, dest is file|dne
      }
    }
  }
  
  mds->balancer->hit_dir(srcdn->dir, META_POP_DWR);
  mds->balancer->hit_dir(destdn->dir, META_POP_DWR);

  // we're golden.
  // everything is xlocked by us, we rule, etc.
  MClientReply *reply = new MClientReply(req, 0);
  mdcache->renamer->file_rename( srcdn, destdn,
				 new C_MDS_CommitRequest(this, req, reply, srcdn->inode,
							 new EString("file rename fixme")) );
}











// ===================================
// TRUNCATE, FSYNC

/*
 * FIXME: this truncate implemention is WRONG WRONG WRONG
 */

void Server::handle_client_truncate(MClientRequest *req, CInode *cur)
{
  // write
  if (!mds->locker->inode_file_write_start(cur, req))
    return;  // fw or (wait for) lock

  // check permissions
  
  // do update
  cur->inode.size = req->args.truncate.length;
  cur->_mark_dirty(); // fixme

  mds->locker->inode_file_write_finish(cur);

  mds->balancer->hit_inode(cur, META_POP_IWR);   

  // start reply
  MClientReply *reply = new MClientReply(req, 0);

  // commit
  commit_request(req, reply, cur,
                 new EString("truncate fixme"));
}



// ===========================
// open, openc, close

void Server::handle_client_open(MClientRequest *req,
				CInode *cur)
{
  int flags = req->args.open.flags;
  int mode = req->args.open.mode;

  dout(7) << "open " << flags << " on " << *cur << endl;
  dout(10) << "open flags = " << flags << "  mode = " << mode << endl;

  // is it a file?
  if (!(cur->inode.mode & INODE_MODE_FILE)) {
    dout(7) << "not a regular file" << endl;
    reply_request(req, -EINVAL);                 // FIXME what error do we want?
    return;
  }

  // auth for write access
  if (mode != FILE_MODE_R && mode != FILE_MODE_LAZY &&
      !cur->is_auth()) {
    int auth = cur->authority().first;
    assert(auth != mds->get_nodeid());
    dout(9) << "open writeable on replica for " << *cur << " fw to auth " << auth << endl;
    
    mdcache->request_forward(req, auth);
    return;
  }


  // hmm, check permissions or something.


  // can we issue the caps they want?
  version_t fdv = mds->locker->issue_file_data_version(cur);
  Capability *cap = mds->locker->issue_new_caps(cur, mode, req);
  if (!cap) return; // can't issue (yet), so wait!

  dout(12) << "open gets caps " << cap_string(cap->pending()) << " for " << req->get_source() << " on " << *cur << endl;

  mds->balancer->hit_inode(cur, META_POP_IRD);

  // reply
  MClientReply *reply = new MClientReply(req, 0);
  reply->set_file_caps(cap->pending());
  reply->set_file_caps_seq(cap->get_last_seq());
  reply->set_file_data_version(fdv);
  reply_request(req, reply, cur);
}


class C_MDS_openc_finish : public Context {
  MDS *mds;
  MClientRequest *req;
  CDentry *dn;
  CInode *newi;
  version_t pv;
public:
  C_MDS_openc_finish(MDS *m, MClientRequest *r, CDentry *d, CInode *ni) :
    mds(m), req(r), dn(d), newi(ni),
    pv(d->get_projected_version()) {}
  void finish(int r) {
    assert(r == 0);

    // link the inode
    dn->get_dir()->link_inode(dn, newi);

    // dirty inode, dn, dir
    newi->mark_dirty(pv);

    // unlock
    mds->locker->dentry_xlock_finish(dn);

    // hit pop
    mds->balancer->hit_inode(newi, META_POP_IWR);

    // ok, do the open.
    mds->server->handle_client_open(req, newi);
  }
};


void Server::handle_client_openc(MClientRequest *req, CInode *diri)
{
  dout(7) << "open w/ O_CREAT on " << req->get_filepath() << endl;

  CDir *dir = 0;
  CInode *in = 0;
  CDentry *dn = 0;
  
  // make dentry and inode, xlock dentry.
  bool excl = (req->args.open.flags & O_EXCL);
  int r = prepare_mknod(req, diri, &dir, &in, &dn, !excl);  // okexist = !excl
  if (r <= 0) 
    return; // wait on something
  assert(dir);
  assert(in);
  assert(dn);

  if (r == 1) {
    // created.
    // it's a file.
    dn->pre_dirty();
    in->inode.mode = 0644;              // FIXME req should have a umask
    in->inode.mode |= INODE_MODE_FILE;

    // prepare finisher
    C_MDS_openc_finish *fin = new C_MDS_openc_finish(mds, req, dn, in);
    EUpdate *le = new EUpdate("openc");
    le->metablob.add_dir_context(dir);
    inode_t *pi = le->metablob.add_primary_dentry(dn, true, in);
    pi->version = dn->get_projected_version();
    
    // log + wait
    mdlog->submit_entry(le);
    mdlog->wait_for_sync(fin);

    /*
      FIXME. this needs to be rewritten when the write capability stuff starts
      getting journaled.  
    */
  } else {
    // exists!
    
    // O_EXCL?
    if (req->args.open.flags & O_EXCL) {
      // fail.
      dout(10) << "O_EXCL, target exists, failing with -EEXIST" << endl;
      reply_request(req, -EEXIST, in);
      return;
    } 
    
    // FIXME: do i need to repin path based existant inode? hmm.
    handle_client_open(req, in);
  }
}














