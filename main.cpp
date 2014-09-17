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

static std::atomic<bool> first_packet(true);
static std::atomic<size_t> gate;
static size_t threads(2/*std::thread::hardware_concurrency()*/), buffers(4), packet_size(65507);
static udp::endpoint local(asio::ip::address_v4::any(), 7868), endpoint(asio::ip::address_v4::loopback(), 7868);
static std::chrono::time_point<std::chrono::high_resolution_clock> begin;
static asio::io_service service(threads);
static udp::socket listening_socket(service);

#ifdef WIN32
unsigned char *allocate_dma_buffer(size_t len) {
  void *ret = VirtualAlloc(nullptr, len, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
  return reinterpret_cast<unsigned char *>(ret);
}
void deallocate_dma_buffer(unsigned char *buf, size_t /*len*/) {
  VirtualFree(buf, 0, MEM_RELEASE);
}
#else
unsigned char *allocate_dma_buffer(size_t len) {
  void *ret = mmap(nullptr, len, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED,
                   -1, 0);
  return reinterpret_cast<unsigned char *>(ret);
}
void deallocate_dma_buffer(unsigned char *buf, size_t len) {
  munmap(buf, len);
}
#endif

struct worker
{
  udp::socket socket; // sockets aren't thread safe, so need one per thread
  udp::endpoint myremote; // need a local thread safe copy
  size_t read_count, write_count, read_bytes;
  std::vector<unsigned char *> read_buffers, write_buffers;
  std::vector<unsigned char *>::iterator read_buffer, write_buffer;
  std::thread thread;
  worker() : worker(0) { }
  worker(size_t myidx) : socket(service, asio::ip::udp::v4()), myremote(endpoint), read_count(0), write_count(0), read_bytes(0), thread(&worker::run, this, myidx) { }
  worker(const worker &) = delete;
  worker(worker &&) : worker(0) {}
  void doread()
  {
    listening_socket.async_receive(asio::buffer(*read_buffer, packet_size), [&](error_code ec, size_t bytes)
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
    });
  }
  void dowrite()
  {
    socket.async_send(asio::buffer(*write_buffer, packet_size), [&](error_code ec, size_t bytes)
    {
      if(!ec)
        ++write_count;
      else
      {
        std::cout << "w @ " << ((void *) *write_buffer) << " " << ec << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if(++write_buffer==write_buffers.end())
        write_buffer=write_buffers.begin();
      dowrite();
    });
  }
  void run(size_t myidx)
  {
    for(size_t n=0; n<buffers; n++)
    {
      read_buffers.push_back(allocate_dma_buffer(packet_size));
      write_buffers.push_back(allocate_dma_buffer(packet_size));
    }
    read_buffer=read_buffers.begin();
    write_buffer=write_buffers.begin();
    //std::cout << ((void *) read_buffers.front()) << "," << ((void *) write_buffers.front()) << std::endl;
    //std::cout << ((void *) *read_buffer) << "," << ((void *) *write_buffer) << std::endl;
    socket.set_option(asio::socket_base::receive_buffer_size(65507));
    socket.set_option(asio::socket_base::send_buffer_size(65507));
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
    doread();
    dowrite();
    service.run();
  }
  void join() { return thread.join(); }
};

int main(int argc, char *argv[])
{
  if(argc==1)
    std::cout << "Usage: " << argv[0] << " ip:port [buffers] [datagram size]\n"
              << "Using localhost:7868 4 65507 as default\n";
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
      buffers=atoi(argv[2]);
    if(argc>=4)
      packet_size=atoi(argv[3]);
  }
  error_code ec;
  if(listening_socket.open(asio::ip::udp::v4(), ec))
  {
    std::cerr << "Failed to open UDP socket\n";
    return 1;
  }
  listening_socket.set_option(asio::socket_base::receive_buffer_size(65507));
  listening_socket.set_option(asio::socket_base::send_buffer_size(65507));
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
  else
    endpoint.port(endpoint.port()+1);
  std::cout << "Listening to " << local << " and sending to " << endpoint << " ...\n";
  //std::cout << "Launch the other side now and press Return when it's ready ...\n";
  //getchar();
  
  std::vector<worker> workers;
  workers.reserve(threads);
  std::cout << "Creating " << workers.capacity() << " workers to read and write\n";
  std::cout << "Press return to exit ...\n";
  gate=workers.capacity()+1;
  for(size_t n=0; n<workers.capacity(); n++)
    workers.emplace_back(n);
  auto temp=begin=std::chrono::high_resolution_clock::now();
  while(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now()-temp).count()<3)
    std::this_thread::yield();
  --gate;
  while(gate)
    std::this_thread::yield();
#if 0
  getchar();
#else
  // Windows blocks so heavily SpeedStep interferes so keep a CPU more busy ...
  while(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now()-begin).count()<10)
    std::this_thread::yield();
#endif
  service.stop();
  for(auto &i: workers)
    i.join();
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
  double rate=bytes;
  rate/=std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count()/1000000000.0;
  rate/=1024*1024;
  std::cout << "And some " << rate << "Mb/sec.\n";
  return 0;
}
