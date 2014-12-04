//#define USE_MEMORY_PRESSURE (8*1024*1024)
//#define ENABLE_WIN32_RIO

#ifdef WIN32
#  define WIN32_LEAN_AND_MEAN 1
#  include <windows.h>
#else
#  include <sys/mman.h>
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>

namespace asio = boost::asio;
using asio::ip::udp;
using boost::system::error_code;
using boost::system::system_category;

#ifdef WIN32
typedef void *native_handle_type;
#else
typedef int native_handle_type;
#endif
typedef std::pair<unsigned char *, native_handle_type> dma_buffer_type;

static std::atomic<bool> first_packet(true);
static std::atomic<size_t> gate;
static size_t threads(1 /*std::thread::hardware_concurrency()*/), buffers(2), packet_size(1460);
static udp::endpoint local(asio::ip::address_v4::any(), 7868), endpoint(asio::ip::address_v4::loopback(), 7868);
static std::chrono::time_point<std::chrono::high_resolution_clock> begin;
static asio::io_service service(threads);
static udp::socket listening_socket(service);

#ifdef WIN32
dma_buffer_type allocate_dma_buffer(size_t len) {
  unsigned char *ret = (unsigned char *) VirtualAlloc(nullptr, len, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
  return std::make_pair(ret, nullptr);
}
void deallocate_dma_buffer(dma_buffer_type buf) {
  VirtualFree(buf.first, 0, MEM_RELEASE);
}
#ifdef ENABLE_WIN32_RIO
RIO_EXTENSION_FUNCTION_TABLE rio;
#endif
#else
dma_buffer_type allocate_dma_buffer(size_t len) {
  static std::atomic<unsigned> count;
  char buffer[PATH_MAX];
  sprintf(buffer, "/tmp/zero-copy-socket-test-%u", ++count);
  int fd=open(buffer, O_CREAT|O_EXCL|O_RDWR
#ifdef O_NOATIME
              |O_NOATIME
#endif
              , 0x180);
  if(-1==fd) return dma_buffer_type();
  if(-1==ftruncate(fd, len))
  {
    unlink(buffer);
    close(fd);
    return dma_buffer_type();
  }
  unsigned char *addr=(unsigned char *) mmap(nullptr, len, PROT_READ|PROT_WRITE, MAP_SHARED
#ifdef MAP_POPULATE
             |MAP_POPULATE
#endif
#ifdef MAP_NOSYNC
             |MAP_NOSYNC
#endif
#ifdef MAP_PREFAULT_READ
             |MAP_PREFAULT_READ
#endif
             , fd, 0);
  unlink(buffer);
  if(!addr)  
  {
    close(fd);
    return dma_buffer_type();
  }
  // Force the kernel to actually allocate storage, otherwise DMA cannot work
  static size_t page_size=getpagesize();
  for(size_t n=0; n<len; n+=page_size)
    addr[n]=1;
  return std::make_pair(addr, fd);
}
void deallocate_dma_buffer(dma_buffer_type buf) {
  munmap(buf.first, 1);
}
#endif

struct worker
{
  udp::socket socket; // sockets aren't thread safe, so need one per thread
  udp::endpoint myremote; // need a local thread safe copy
  size_t read_count, write_count, read_bytes;
  std::vector<dma_buffer_type> read_buffers, write_buffers;
  std::vector<dma_buffer_type>::iterator read_buffer, write_buffer;
#ifdef __linux__
  std::vector<std::pair<int, int>> read_buffer_fifos, write_buffer_fifos;
  std::vector<std::pair<int, int>>::iterator read_buffer_fifo, write_buffer_fifo;
#endif
#ifdef WIN32
  HANDLE lsocketh;
#endif
  std::thread thread;
  worker() : worker(0) { }
  worker(size_t myidx) : socket(service, asio::ip::udp::v4()), myremote(endpoint), read_count(0), write_count(0), read_bytes(0), thread(&worker::run, this, myidx)
#ifdef WIN32
    , lsocketh(nullptr)
#endif
  { }
  worker(const worker &) = delete;
  worker(worker &&) : worker(0) {}
  void doread()
  {
    auto handle_read=[&](error_code ec, size_t bytes)
    {
      if(!ec)
      {
        if(first_packet)
        {
          first_packet=false;
          begin=std::chrono::high_resolution_clock::now();
        }
        ++read_count;
        read_bytes+=bytes;
      }
      else
        std::cout << "r " << ec << std::endl;
      if(++read_buffer==read_buffers.end())
        read_buffer=read_buffers.begin();
      doread();
    };
    listening_socket.async_receive(asio::buffer(read_buffer->first, 65535), handle_read);
  }
  void dowrite()
  {
    auto handle_write=[&](error_code ec, size_t bytes)
    {
      if(!ec)
        ++write_count;
      else
      {
        std::cout << "w @ " << ((void *) write_buffer->first) << " " << ec << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if(++write_buffer==write_buffers.end())
        write_buffer=write_buffers.begin();
#ifdef __linux__
      if(++write_buffer_fifo==write_buffer_fifos.end())
        write_buffer_fifo=write_buffer_fifos.begin();
#endif
      dowrite();
    };
#if defined(__linux__) && 1
    // Linux needs to be encouraged to do zero copy
    if(buffers>1)
    {
      loff_t o=0;
      // Accumulate gather buffers into write fifo
      if(-1==splice(write_buffer->second, &o, write_buffer_fifo->second, nullptr, packet_size, SPLICE_F_MOVE))
      {
        int e=errno;
        std::cerr << "splice1 failed with " << e << std::endl;
        abort();
      }
      // Issue the packet send
      if(-1==splice(write_buffer_fifo->first, nullptr, socket.native_handle(), nullptr, packet_size, SPLICE_F_MOVE|SPLICE_F_NONBLOCK))
      {
        int e=errno;
        if(ECONNREFUSED==e)
        {
          error_code ec(e, system_category());
          handle_write(ec, 0);
          return;
        }
        std::cerr << "splice2 failed with " << e << std::endl;
        abort();
      }
      socket.async_send(asio::null_buffers(), handle_write);
    }
    else
#endif
    {
      socket.async_send(asio::buffer(write_buffer->first, packet_size), handle_write);
    }
  }
  void run(size_t myidx)
  {
    for(size_t n=0; n<buffers; n++)
    {
      read_buffers.push_back(allocate_dma_buffer(65536));
      write_buffers.push_back(allocate_dma_buffer(packet_size));
#ifdef __linux__
      int fds[2];
      pipe2(fds, O_NONBLOCK);
      read_buffer_fifos.push_back(std::make_pair(fds[0], fds[1]));
      pipe2(fds, O_NONBLOCK);
      write_buffer_fifos.push_back(std::make_pair(fds[0], fds[1]));
#endif
    }
    read_buffer=read_buffers.begin();
    write_buffer=write_buffers.begin();
#ifdef __linux__
    read_buffer_fifo=read_buffer_fifos.begin();
    write_buffer_fifo=write_buffer_fifos.begin();
#endif
    //std::cout << ((void *) read_buffers.front()) << "," << ((void *) write_buffers.front()) << std::endl;
    //std::cout << ((void *) *read_buffer) << "," << ((void *) *write_buffer) << std::endl;
    socket.set_option(asio::socket_base::receive_buffer_size(65487));
    socket.set_option(asio::socket_base::send_buffer_size(65487));
#if 0
    asio::socket_base::reuse_address option(true);
    socket.set_option(option);
    error_code ec;
    if(socket.bind(mylocal, ec))
    {
      std::cerr << "Failed to bind socket copy\n";
      abort();
    }
#endif
    socket.connect(myremote);
    --gate;
    while(gate)
      std::this_thread::yield();
#if 0
    // Single writer many reader
    if(!myidx)
      dowrite();
    else
      doread();
#else
    doread();
    dowrite();
#endif
    service.run();
  }
  void join() { return thread.join(); }
};

int main(int argc, char *argv[])
{
  if(argc==1)
    std::cout << "Usage: " << argv[0] << " ip:port [use zero copy] [datagram size]\n"
              << "Using localhost:7868 1 1500 as default\n";
  else
  {
    if(argc>=2)
    {
      std::string a(argv[1]);
      auto colon=a.rfind(':');
      if(colon!=(size_t)-1)
      {
        endpoint.port(atoi(&a[colon+1]));
        a.resize(colon);
      }
      endpoint.address(asio::ip::address::from_string(a));
    }
    if(argc>=3)
      buffers=atoi(argv[2])!=0 ? 2 : 1;
    if(argc>=4)
      packet_size=atoi(argv[3]);
  }
  error_code ec;
  if(listening_socket.open(asio::ip::udp::v4(), ec))
  {
    std::cerr << "Failed to open UDP socket\n";
    return 1;
  }
  listening_socket.set_option(asio::socket_base::receive_buffer_size(65487));
  listening_socket.set_option(asio::socket_base::send_buffer_size(65487));
  // Try to bind to this or next port
  if(listening_socket.bind(local, ec))
  {
    std::cerr << "Failed to bind to " << local.port() << " due to " << ec << "\n";
    local.port(local.port()+1);
    if(listening_socket.bind(local, ec))
    {
      std::cerr << "Failed to bind to " << local.port() << " due to " << ec << "\n";
      return 1;
    }
  }
  //else if(argc<2)
  //  endpoint.port(endpoint.port()+1);
  std::cout << "Listening to " << local << " and sending to " << endpoint << " ...\n";
  //std::cout << "Launch the other side now and press Return when it's ready ...\n";
  //getchar();
#ifdef ENABLE_WIN32_RIO
  {
    GUID functionTableId = WSAID_MULTIPLE_RIO;
    DWORD dwBytes = 0;
    if(0 != WSAIoctl(
      listening_socket.native_handle(),
      SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER,
      &functionTableId,
      sizeof(GUID),
      (void**) &rio,
      sizeof(rio),
      &dwBytes,
      0,
      0))
    {
      DWORD lastError = ::GetLastError();
      std::cerr << "RIO fetch failed with " << lastError << std::endl;
      abort();
    }
  }
#endif

#ifdef USE_MEMORY_PRESSURE
  std::atomic<bool> mem_pressure_done(false);
  std::thread mem_pressure([&mem_pressure_done]{
    std::vector<char> a(USE_MEMORY_PRESSURE);
    while(!mem_pressure_done)
    {
      std::fill(a.begin(), a.end(), 78);
    }
  });
#endif
  std::atomic<bool> watchdog_done(false);
  std::condition_variable watchdog_cv;
  std::thread watchdog([&watchdog_done, &watchdog_cv]{
    std::mutex m;
    std::unique_lock<decltype(m)> l(m);
    if(!watchdog_cv.wait_for(l, std::chrono::minutes(5), [&watchdog_done]{return !!watchdog_done; }))
    {
      std::cerr << "Timed out" << std::endl;
      abort();
    }
  });
  std::vector<worker> workers;
  workers.reserve(threads);
  std::cout << "Creating " << workers.capacity() << " workers to read and write\n";
  std::cout << "Press return to exit ...\n";
  gate=workers.capacity()+1;
  for(size_t n=0; n<workers.capacity(); n++)
    workers.emplace_back(n);
  --gate;
  while(gate)
    std::this_thread::yield();
  std::thread printspeed([&]{
    while(!gate)
    {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      auto end=std::chrono::high_resolution_clock::now();
      std::cout << "\nWorkers did:\n";
      size_t reads=0, writes=0, bytes=0;
      for(auto &i: workers)
      {
        std::cout << "  " << i.read_count << " reads and " << i.write_count << " writes\n";
        reads+=i.read_count;
        writes+=i.write_count;
        bytes+=i.read_bytes;
      }
      std::cout << "Making a total of " << reads << " reads and " << writes << " writes of " << bytes << " bytes\n";
      double rate=(double) bytes;
      rate/=std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count()/1000000000.0;
      rate/=1024*1024;
      std::cout << "And some " << rate << "Mb/sec.\n";
      for(auto &i: workers)
      {
        i.read_count=i.write_count=i.read_bytes=0;
      }
      begin=std::chrono::high_resolution_clock::now();
    }
  });
  getchar();
  gate=1;
  service.stop();
  printspeed.join();
  for(auto &i: workers)
    i.join();
  watchdog_done=true;
  watchdog_cv.notify_all();
  watchdog.join();
#ifdef USE_MEMORY_PRESSURE
  mem_pressure_done=true;
  mem_pressure.join();
#endif
  return 0;
}
