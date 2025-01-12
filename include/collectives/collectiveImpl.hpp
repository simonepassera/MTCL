#ifndef COLLECTIVEIMPL_HPP
#define COLLECTIVEIMPL_HPP

#include <iostream>
#include <map>
#include <vector>

#include "../handle.hpp"
#include "../utils.hpp"


enum ImplementationType {
    GENERIC,
    MPI,
    UCC
};


/**
 * @brief Interface for transport-specific network functionalities for collective
 * operations. Subclasses specify different behaviors depending on the specific
 * transport used to implement the collective operations and on the specific type
 * of collective.
 * 
 */
class CollectiveImpl {
protected:
    std::vector<Handle*> participants;
	size_t nparticipants;
	int uniqtag=-1;
	int rank;   // team rank 
	
    //TODO: 
    // virtual bool canSend() = 0;
    // virtual bool canReceive() = 0;

protected:
    ssize_t probeHandle(Handle* realHandle, size_t& size, const bool blocking=true) {
		if (realHandle->probed.first) { // previously probed, return 0 if EOS received
			size=realHandle->probed.second;
			return (size ? sizeof(size_t) : 0);
		}
        if (!realHandle) {
			MTCL_PRINT(100, "[internal]:\t", "CollectiveImpl::probeHandle EBADF\n");
            errno = EBADF; // the "communicator" is not valid or closed
            return -1;
        }
		if (realHandle->closed_rd) return 0;

		// reading the header to get the size of the message
		ssize_t r;
		if ((r=realHandle->probe(size, blocking))<=0) {
			switch(r) {
			case 0: {
				realHandle->close(true, true);
				return 0;
			}
			case -1: {				
				if (errno==ECONNRESET) {
					realHandle->close(true, true);
					return 0;
				}
				if (errno==EWOULDBLOCK || errno==EAGAIN) {
					errno = EWOULDBLOCK;
					return -1;
				}
			}}
			return r;
		}
		realHandle->probed={true,size};
		if (size==0) { // EOS received
			realHandle->close(false, true);
			return 0;
		}
		return r;		
	}

    ssize_t receiveFromHandle(Handle* realHandle, void* buff, size_t size) {
		size_t sz;
		if (!realHandle->probed.first) {
			// reading the header to get the size of the message
			ssize_t r;
			if ((r=probeHandle(realHandle, sz, true))<=0) {
				return r;
			}
		} else {
			if (!realHandle) {
				MTCL_PRINT(100, "[internal]:\t", "CollectiveImpl::receiveFromHandle EBADF\n");
				errno = EBADF; // the "communicator" is not valid or closed
				return -1;
			}
			if (realHandle->closed_rd) return 0;
		}
		if ((sz=realHandle->probed.second)>size) {
			MTCL_ERROR("[internal]:\t", "CollectiveImpl::receiveFromHandle ENOMEM, receiving less data\n");
			errno=ENOMEM;
			return -1;
		}	   
		realHandle->probed={false,0};
		return realHandle->receive(buff, std::min(sz,size));
    }


public:
    CollectiveImpl(std::vector<Handle*> participants, size_t nparticipants, int rank, int uniqtag)
		: participants(participants), nparticipants(nparticipants), uniqtag(uniqtag), rank(rank) {
        // for(auto& h : participants) h->incrementReferenceCounter();
    }

    /**
     * @brief Checks if any of the participants has something ready to be read.
     * 
     * @return true, if at least one participant has something to read. False
     * otherwise 
     */
    virtual bool peek() {
        //NOTE: the basic implementation returns true as soon as one of the
        //      participants has something to receive. Some protocols may require
        //      to override this method in order to correctly "peek" for messages 
        for(auto& h : participants) {
            if(h->peek()) return true;
        }

        return false;
    }
	
    virtual ssize_t probe(size_t& size, const bool blocking=true) = 0;
    virtual ssize_t send(const void* buff, size_t size) = 0;
    virtual ssize_t receive(void* buff, size_t size) = 0;
    virtual void close(bool close_wr=true, bool close_rd=true) = 0;

	virtual int getTeamRank() {	return rank; }
    virtual int getTeamPartitionSize(size_t buffcount) {
        int partition = buffcount / nparticipants;
		int r = buffcount % nparticipants;

		if (r && (rank < r)) partition++;

        return partition;
    }

	
    virtual ssize_t sendrecv(const void* sendbuff, size_t sendsize, void* recvbuff, size_t recvsize, size_t datasize = 1) {
        MTCL_PRINT(100, "[internal]:\t", "CollectiveImpl::sendrecv invalid operation for the collective\n");
        errno = EINVAL;
        return -1;
    }

    virtual void finalize(bool, std::string name="") {return;}

    virtual ~CollectiveImpl() {}
};

/**
 * @brief Generic implementation of Broadcast collective using low-level handles.
 * This implementation is intended to be used by those transports that do not have
 * an optimized implementation of the Broadcast collective. This implementation
 * can be selected using the \b BROADCAST type and the \b GENERIC implementation,
 * provided, respectively, by @see CollectiveType and @see ImplementationType. 
 * 
 */
class BroadcastGeneric : public CollectiveImpl {
protected:
    bool root;
    
public:
    ssize_t probe(size_t& size, const bool blocking=true) {
		MTCL_ERROR("[internal]:\t", "Broadcast::probe operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t send(const void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Broadcast::send operation not supported, you must use the sendrecv method\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t receive(void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Broadcast::receive operation not supported, you must use the sendrecv method\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t sendrecv(const void* sendbuff, size_t sendsize, void* recvbuff, size_t recvsize, size_t datasize = 1) {
        if(root) {
            for(auto& h : participants) {
                if(h->send(sendbuff, sendsize) < 0) {
                    errno = ECONNRESET;
                    return -1;
                }
            }
			if (recvbuff)
				memcpy(recvbuff, sendbuff, sendsize);
			
            return sendsize;
        }
        else {
            auto h = participants.at(0);
            ssize_t res = receiveFromHandle(h, (char*)recvbuff, recvsize);
            if(res == 0) h->close(true, false);

            return res;
        }
    }

    void close(bool close_wr=true, bool close_rd=true) {
        // Root process can issue an explicit close to all its non-root processes.
        if(root) {
            for(auto& h : participants) h->close(true, false);
            return;
        }
    }

public:
    BroadcastGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag)
		: CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

};

/**
 * @brief Generic implementation of Scatter collective using low-level handles.
 * This implementation is intended to be used by those transports that do not have
 * an optimized implementation of the Scatter collective. This implementation
 * can be selected using the \b SCATTER type and the \b GENERIC implementation,
 * provided, respectively, by @see CollectiveType and @see ImplementationType. 
 * 
 */
class ScatterGeneric : public CollectiveImpl {
protected:
    bool root;

public:
    ssize_t probe(size_t& size, const bool blocking=true) {
		MTCL_ERROR("[internal]:\t", "Scatter::probe operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t send(const void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Scatter::send operation not supported, you must use the sendrecv method\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t receive(void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Scatter::receive operation not supported, you must use the sendrecv method\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t sendrecv(const void* sendbuff, size_t sendsize, void* recvbuff, size_t recvsize, size_t datasize = 1) {
        MTCL_TCP_PRINT(100, "sendrecv, sendsize=%ld, recvsize=%ld, datasize=%ld, nparticipants=%ld\n", sendsize, recvsize, datasize, nparticipants);

        if (recvbuff == nullptr) {
            MTCL_ERROR("[internal]:\t","receive buffer == nullptr\n");
            errno=EFAULT;
            return -1;
        }

        if(root) {
            if (sendbuff == nullptr) {
                MTCL_ERROR("[internal]:\t","sender buffer == nullptr\n");
                errno=EFAULT;
                return -1;
            }

            if (sendsize % datasize != 0) {
                errno=EINVAL;
                return -1;
            }

            size_t datacount = sendsize / datasize;

            size_t sendcount = (datacount / nparticipants) * datasize;
            size_t rcount = (datacount % nparticipants);

            size_t selfsendcount = sendcount;

            if (rcount > 0) {
                selfsendcount += datasize;
                rcount--;
            }

            if (recvsize < selfsendcount) {
                MTCL_ERROR("[internal]:\t","receive buffer too small %ld instead of %ld\n", recvsize, selfsendcount);
                errno=EINVAL;
                return -1;
            }

            memcpy((char*)recvbuff, sendbuff, selfsendcount);
            sendbuff = (char*)sendbuff + selfsendcount;
            
            size_t chunksize;
            
            for (size_t i = 0; i < (nparticipants -1); i++) {
                chunksize = sendcount;

                if (rcount > 0) {
                    chunksize += datasize;
                    rcount--;
                }

                if(participants.at(i)->send(sendbuff, chunksize) < 0) {
                    errno = ECONNRESET;
                    return -1;
                }

                sendbuff = (char*)sendbuff + chunksize;
            }
            
            return selfsendcount;
        } else {
            auto h = participants.at(0);
            ssize_t res = receiveFromHandle(h, (char*)recvbuff, recvsize);
            if(res == 0) h->close(true, false);

            return res;
        }
    }

    void close(bool close_wr=true, bool close_rd=true) {
        // Root process can issue an explicit close to all its non-root processes.
        if(root) {
            for(auto& h : participants) h->close(true, false);
            return;
        }
    }

public:
    ScatterGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag)
		: CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

};

class FanInGeneric : public CollectiveImpl {
private:
    ssize_t probed_idx = -1;
    bool root;

public:
	// OK
    ssize_t probe(size_t& size, const bool blocking=true) {
        
        ssize_t res = -1;
        auto iter = participants.begin();
        while(res == -1 && !participants.empty()) {
            auto h = *iter;
            res = h->probe(size, false);
            // The handle sent EOS, we remove it from participants and go on
            // looking for a "real" message
            if(res > 0 && size == 0) {
                iter = participants.erase(iter);
                res = -1;
                h->close(true, true);
                if(iter == participants.end()) {
                    if(blocking) {
                        iter = participants.begin();
                        continue;
                    }
                    else break;
                }
            }
            if(res > 0) {
                probed_idx = iter - participants.begin();
				h->probed={true, size};
            }
            iter++;
            if(iter == participants.end()) {
                if(blocking)
                    iter = participants.begin();
                else break;
            }
        }

        // All participants have closed their connection, we "notify" the HandleUser
        // that an EOS has been received for the entire group
        if(participants.empty()) {
            size = 0;
            res = sizeof(size_t);
        }

        return res;
    }

    ssize_t send(const void* buff, size_t size) {
        ssize_t r;
        for(auto& h : participants) {
            if((r = h->send(buff, size)) < 0) return r;
        }

        return size;
    }

    ssize_t receive(void* buff, size_t size) {
        // I already probed one of the handles, I must receive from the same one
        ssize_t r;
        auto h = participants.at(probed_idx);

        if((r = h->receive(buff, size)) <= 0)
            return -1;
        h->probed = {false,0};

        probed_idx = -1;

        return r;
    }

    void close(bool close_wr=true, bool close_rd=true) {
        // Non-root process can send EOS to root and go on.
        if(!root) {
            auto h = participants.at(0);
            h->close(true, false);
            return;
        }	
    }

public:
    FanInGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag)
		: CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

};


class FanOutGeneric : public CollectiveImpl {
private:
    size_t current = 0;
    bool root;

public:
    ssize_t probe(size_t& size, const bool blocking=true) {
        if(participants.empty()) {
            errno = ECONNRESET;
            return -1;
        }

        auto h = participants.at(0);
        ssize_t res = h->probe(size, blocking);
        // EOS
        if(res > 0 && size == 0) {
            participants.pop_back();
            h->close(true, true);
        }
		if (res > 0) 
			h->probed={true, size};
        return res;
    }

    ssize_t send(const void* buff, size_t size) {
        size_t count = participants.size();
        auto h = participants.at(current);
        
        int res = h->send(buff, size);

        ++current %= count;

        return res;
    }

    ssize_t receive(void* buff, size_t size) {
        auto h = participants.at(0);
        ssize_t res = h->receive(buff, size);
        h->probed = {false, 0};

        return res;
    }

    void close(bool close_wr=true, bool close_rd=true) {		
        // Root process can issue the close to all its non-root processes.
        if(root) {
            for(auto& h : participants) h->close(true, false);
            return;
        }
    }

public:
    FanOutGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag)
		: CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

};


class GatherGeneric : public CollectiveImpl {
private:
    bool root;
public:
    GatherGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag) :
        CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

    ssize_t probe(size_t& size, const bool blocking=true) {
		MTCL_ERROR("[internal]:\t", "Gather::probe operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t receive(void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Gather::receive operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t send(const void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Gather::send operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t sendrecv(const void* sendbuff, size_t sendsize, void* recvbuff, size_t recvsize, size_t datasize = 1) {
		MTCL_TCP_PRINT(100, "sendrecv, sendsize=%ld, recvsize=%ld, datasize=%ld, nparticipants=%ld\n", sendsize, recvsize, datasize, nparticipants);

        if (sendbuff == nullptr) {
            MTCL_ERROR("[internal]:\t","sender buffer == nullptr\n");
            errno=EFAULT;
            return -1;
        }

        if (recvsize % datasize != 0) {
            errno=EINVAL;
            return -1;
        }

        size_t datacount = recvsize / datasize;

        size_t recvcount = (datacount / nparticipants) * datasize;
        size_t rcount = (datacount % nparticipants);
		
        if(root) {
            size_t selfrecvcount = recvcount;

            if (rcount > 0) {
                selfrecvcount += datasize;
            }

            if (sendsize < selfrecvcount) {
                MTCL_ERROR("[internal]:\t","sending buffer too small %ld instead of %ld\n", sendsize, selfrecvcount);
                errno=EINVAL;
                return -1;
            }

            if (recvbuff == nullptr) {
                MTCL_ERROR("[internal]:\t","receive buffer == nullptr\n");
                errno=EFAULT;
                return -1;
            }

            memcpy((char*)recvbuff, sendbuff, selfrecvcount);

            size_t chunksize, displ = selfrecvcount;
            ssize_t return_value;
            
            for (size_t i = 0; i < (nparticipants - 1); i++) {
                if (rcount && ((i + 1) < rcount)) {
                    chunksize = recvcount + datasize;
                } else {
                    chunksize = recvcount;
                }

                // Receive data
                if ((return_value = receiveFromHandle(participants.at(i), (char*)recvbuff + displ, chunksize)) <= 0) {
                    return return_value;
                }

                displ += chunksize;
            }
            
            return selfrecvcount;
        } else {
            size_t chunksize;

            if (rcount && (CollectiveImpl::rank < (int)rcount)) {
                chunksize = recvcount + datasize;
            } else {
                chunksize = recvcount;
            }

            if (chunksize > sendsize) {
                MTCL_ERROR("[internal]:\t","sending buffer too small %ld instead of %ld\n", sendsize, chunksize);
                errno = EINVAL;
                return -1;
            }

            auto h = participants.at(0);

            if(h->send(sendbuff, chunksize) < 0) {
                errno = ECONNRESET;
                return -1;
            }

            return chunksize;
        }
    }

    void close(bool close_wr=true, bool close_rd=true) {        
        for(auto& h : participants) {
            h->close(true, false);
        }

        return;
    }
    
    ~GatherGeneric () {}
};

class AllGatherGeneric : public CollectiveImpl {
private:
    bool root;
public:
    AllGatherGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag) :
        CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

    ssize_t probe(size_t& size, const bool blocking=true) {
		MTCL_ERROR("[internal]:\t", "Gather::probe operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t receive(void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Gather::receive operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t send(const void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Gather::send operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t sendrecv(const void* sendbuff, size_t sendsize, void* recvbuff, size_t recvsize, size_t datasize = 1) {
		MTCL_TCP_PRINT(100, "sendrecv, sendsize=%ld, recvsize=%ld, datasize=%ld, nparticipants=%ld\n", sendsize, recvsize, datasize, nparticipants);

        if (sendbuff == nullptr) {
            MTCL_ERROR("[internal]:\t","sender buffer == nullptr\n");
            errno=EFAULT;
            return -1;
        }

        if (recvbuff == nullptr) {
                MTCL_ERROR("[internal]:\t","receive buffer == nullptr\n");
                errno=EFAULT;
                return -1;
        }

        if (recvsize % datasize != 0) {
            errno=EINVAL;
            return -1;
        }

        size_t datacount = recvsize / datasize;

        size_t recvcount = (datacount / nparticipants) * datasize;
        size_t rcount = (datacount % nparticipants);
		
        if(root) {
            size_t selfrecvcount = recvcount;

            if (rcount > 0) {
                selfrecvcount += datasize;
            }

            if (sendsize < selfrecvcount) {
                MTCL_ERROR("[internal]:\t","sending buffer too small %ld instead of %ld\n", sendsize, selfrecvcount);
                errno=EINVAL;
                return -1;
            }

            memcpy((char*)recvbuff, sendbuff, selfrecvcount);

            size_t chunksize, displ = selfrecvcount;
            ssize_t return_value;
            
            for (size_t i = 0; i < (nparticipants - 1); i++) {
                if (rcount && ((i + 1) < rcount)) {
                    chunksize = recvcount + datasize;
                } else {
                    chunksize = recvcount;
                }

                // Receive data
                if ((return_value = receiveFromHandle(participants.at(i), (char*)recvbuff + displ, chunksize)) <= 0) {
                    return return_value;
                }

                displ += chunksize;
            }

            for(auto h : participants) {
                if(h->send(recvbuff, recvsize) < 0) {
                    errno = ECONNRESET;
                    return -1;
                }
            }
            
            return selfrecvcount;
        } else {
            size_t chunksize;

            if (rcount && (CollectiveImpl::rank < (int)rcount)) {
                chunksize = recvcount + datasize;
            } else {
                chunksize = recvcount;
            }

            if (chunksize > sendsize) {
                MTCL_ERROR("[internal]:\t","sending buffer too small %ld instead of %ld\n", sendsize, chunksize);
                errno = EINVAL;
                return -1;
            }

            auto h = participants.at(0);

            if(h->send(sendbuff, chunksize) < 0) {
                errno = ECONNRESET;
                return -1;
            }

            if (receiveFromHandle(h, (char*)recvbuff, recvsize) == 0)
                h->close(true, false);

            return chunksize;
        }
    }

    void close(bool close_wr=true, bool close_rd=true) {        
        for(auto& h : participants) {
            h->close(true, false);
        }

        return;
    }
    
    ~AllGatherGeneric () {}
};

class AlltoallGeneric : public CollectiveImpl {
private:
    bool root;
public:
    AlltoallGeneric(std::vector<Handle*> participants, size_t nparticipants, bool root, int rank, int uniqtag) :
        CollectiveImpl(participants, nparticipants, rank, uniqtag), root(root) {}

    ssize_t probe(size_t& size, const bool blocking=true) {
		MTCL_ERROR("[internal]:\t", "Alltoall::probe operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t receive(void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Alltoall::receive operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t send(const void* buff, size_t size) {
        MTCL_ERROR("[internal]:\t", "Alltoall::send operation not supported\n");
		errno=EINVAL;
        return -1;
    }

    ssize_t sendrecv(const void* sendbuff, size_t sendsize, void* recvbuff, size_t recvsize, size_t datasize = 1) {
		MTCL_TCP_PRINT(100, "sendrecv, sendsize=%ld, recvsize=%ld, datasize=%ld, nparticipants=%ld\n", sendsize, recvsize, datasize, nparticipants);

        if (sendbuff == nullptr) {
            MTCL_ERROR("[internal]:\t","sender buffer == nullptr\n");
            errno = EFAULT;
            return -1;
        }

        if (recvbuff == nullptr) {
            MTCL_ERROR("[internal]:\t","receive buffer == nullptr\n");
            errno = EFAULT;
            return -1;
        }

        if (sendsize % datasize != 0) {
            errno = EINVAL;
            return -1;
        }

        size_t datacount = sendsize / datasize;
        size_t sendcount = (datacount / nparticipants) * datasize;
        size_t rcount = datacount % nparticipants;

        size_t selfrecvcount = (sendcount + ((rcount && (rank < (int)rcount)) ? datasize : 0)) * nparticipants;

        if (recvsize < selfrecvcount) {
            MTCL_ERROR("[internal]:\t","receive buffer too small %ld instead of %ld\n", recvsize, selfrecvcount);
            errno = EINVAL;
            return -1;
        }
		
        if(root) {
            char *allsendbuff = new char[sendsize * (nparticipants - 1)];
            int return_value;
            
            for (size_t i = 0; i < (nparticipants - 1); i++) {
                if ((return_value = receiveFromHandle(participants.at(i), (char*)allsendbuff + (i * sendsize), sendsize)) <= 0) {
                    return return_value;
                }
            }

            size_t chunksize, offset, displ = 0;
                
            for (size_t i = 0; i < nparticipants; i++) {
                chunksize = sendcount;
                    
                if (rcount > 0) {
                    chunksize += datasize;
                    rcount--;
                }

                char *chunkbuff;

                if (i == 0)
                    chunkbuff = (char*)recvbuff;
                else
                    chunkbuff = new char[chunksize * nparticipants];

                memcpy(chunkbuff, (char*)sendbuff + displ, chunksize);

                offset = chunksize;

                for (size_t j = 0; j < (nparticipants -1); j++) {
                    memcpy(chunkbuff + offset, allsendbuff + (j * sendsize) + displ, chunksize);
                    offset += chunksize;
                }

                displ += chunksize;

                if (i != 0) {
                    if(participants.at(i - 1)->send(chunkbuff, chunksize * nparticipants) < 0) {
                        errno = ECONNRESET;
                        return -1;
                    }

                    delete [] chunkbuff;
                }
            }

            delete [] allsendbuff;
            
            return selfrecvcount;
        } else {
            auto h = participants.at(0);

            if(h->send(sendbuff, sendsize) < 0) {
                errno = ECONNRESET;
                return -1;
            }

            if (receiveFromHandle(h, (char*)recvbuff, recvsize) == 0)
                h->close(true, false);

            return selfrecvcount;
        }
    }

    void close(bool close_wr=true, bool close_rd=true) {        
        for(auto& h : participants) {
            h->close(true, false);
        }

        return;
    }
    
    ~AlltoallGeneric () {}
};

#endif //COLLECTIVEIMPL_HPP
