#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <nstack/nstack.h>

class net_client;

class databuf
{
  public:
	class entry
	{
	  public:
		std::shared_ptr<net_client> client;
		nstack_queue_entry nqe;
		void *ptr;
		size_t len;
		size_t off = 0;
		entry(std::shared_ptr<net_client> client, nstack_queue_entry *nqe, void *ptr, size_t len)
		  : client(client)
		  , nqe(nqe ? *nqe : nstack_queue_entry{})
		  , ptr(ptr)
		  , len(len)
		{
		}

		size_t rem()
		{
			return len - off;
		}

		void *next_ptr(size_t addoff)
		{
			void *ret = (void *)((char *)ptr + off + addoff);
			return ret;
		}

		void complete();

		bool consume(size_t tlen)
		{
			off += tlen;
			if(off >= len) {
				complete();
				return true;
			}
			return false;
		}
	};

	class databufptr
	{
	  public:
		void *ptr;
		size_t len;
		databufptr(void *ptr, size_t len)
		  : ptr(ptr)
		  , len(len)
		{
		}
	};

  private:
	std::mutex lock;
	std::list<entry *> entries;
	size_t have_read = 0;
	size_t amount;

  public:
	databuf()
	{
	}
	void append(std::shared_ptr<net_client> client, nstack_queue_entry *nqe, void *ptr, size_t len)
	{
		std::lock_guard<std::mutex> _lg(lock);
		// fprintf(stderr, "APPENDING %ld\n", len);
		entries.push_back(new entry(client, nqe, ptr, len));
		amount += len;
	}

	size_t pending()
	{
		std::lock_guard<std::mutex> _lg(lock);
		return amount;
	}

	databufptr get_next(size_t max)
	{
		std::lock_guard<std::mutex> _lg(lock);
		// fprintf(stderr, "GET_NEXT: %ld :: %ld\n", max, have_read);
		size_t off = have_read;
		for(auto entry : entries) {
			// entry *entry = entries.front();
			size_t thisrem = entry->rem();
			if(thisrem <= off) {
				off -= thisrem;
				continue;
			}

			size_t amount = max;
			if(amount > thisrem - off)
				amount = thisrem - off;
			databufptr dbp(entry->next_ptr(off), amount);
			have_read += amount;
#if 0
			fprintf(stderr, "GET_NEXT RET %ld (%p %p %ld) :: ", amount, entry, dbp.ptr, entry->off);
			for(int i = 0; i < amount; i++)
				fprintf(stderr, "%x ", *((unsigned char *)dbp.ptr + i));
			fprintf(stderr, "\n");
#endif
			return dbp;
		}
		return databufptr(NULL, 0);
	}

	void remove(size_t count)
	{
		std::lock_guard<std::mutex> _lg(lock);
		// fprintf(stderr, "REMOVING %ld (%ld)\n", count, have_read);
		size_t len = count;
		while(len > 0) {
			assert(!entries.empty());
			entry *entry = entries.front();
			size_t thislen = len;
			if(thislen > entry->rem())
				thislen = entry->rem();
			if(entry->consume(thislen)) {
				entries.pop_front();
				delete entry;
			}
			len -= thislen;
		}
		assert(have_read >= count);
		have_read -= count;
		amount -= count;
	}

	void reset()
	{
		std::lock_guard<std::mutex> _lg(lock);
		// fprintf(stderr, "RESETTING\n");
		have_read = 0;
	}
};
