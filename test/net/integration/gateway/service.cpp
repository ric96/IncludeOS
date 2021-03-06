// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2016-2017 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <service>
#include <net/super_stack.hpp>
#include <net/inet>

using namespace net;

int pongs = 0;

void verify() {
  static int i = 0;
  if (++i == 9) printf("SUCCESS\n");
}

inline void test_tcp_conntrack();

void Service::start()
{
  static auto& eth0   = Super_stack::get(0);
  static auto& eth1   = Super_stack::get(1);
  static auto& host1  = Super_stack::get(2);
  static auto& host2  = Super_stack::get(3);

  INFO("Ping", "host1 => host2 (%s)", host2.ip_addr().to_string().c_str());
  host1.icmp().ping(host2.ip_addr(), [](auto reply) {
    CHECKSERT(reply, "Got pong from %s", host2.ip_addr().to_string().c_str());
    pongs++;
    verify();
  }, 1);

  INFO("Ping", "host1 => eth0 (%s)", eth0.ip_addr().to_string().c_str());
  host1.icmp().ping(eth0.ip_addr(), [](auto reply) {
    CHECKSERT(reply, "Got pong from %s", eth0.ip_addr().to_string().c_str());
    pongs++;
    verify();
  }, 1);

  INFO("Ping", "host1 => eth1 (%s)", eth1.ip_addr().to_string().c_str());
  host1.icmp().ping(eth1.ip_addr(), [](auto reply) {
    CHECKSERT(reply, "Got pong from %s", eth1.ip_addr().to_string().c_str());
    pongs++;
    verify();
  }, 1);

  INFO("Ping", "host2 => host1 (%s)", host1.ip_addr().to_string().c_str());
  host2.icmp().ping(host1.ip_addr(), [](auto reply) {
    CHECKSERT(reply, "Got pong from %s", host1.ip_addr().to_string().c_str());
    pongs++;
    verify();
  }, 1);

  INFO("TCP", "host2 => listen:5000");
  host2.tcp().listen(5000, [](auto /*conn*/) {
  });

  INFO("TCP", "host1 => host2 (%s:%i)", host2.ip_addr().to_string().c_str(), 5000);
  host1.tcp().connect({host2.ip_addr(), 5000}, [](auto conn) {
    CHECKSERT(conn, "Connection established with %s:%i", host2.ip_addr().to_string().c_str(), 5000);
    verify();
  });

  INFO("TCP", "host1 => eth0 (%s:%i)", eth0.ip_addr().to_string().c_str(), 5001);
  host1.tcp().connect({eth0.ip_addr(), 5001}, [](auto conn) {
    CHECKSERT(conn, "Connection established with %s:%i", eth0.ip_addr().to_string().c_str(), 5001);
    verify();
  });

  INFO("TCP", "host2 => listen:1337");
  host2.tcp().listen(1337, [](auto) {});
  INFO("TCP", "host1 => eth0 (%s:%i)", eth0.ip_addr().to_string().c_str(), 1337);
  host1.tcp().connect({eth0.ip_addr(), 1337}, [](auto conn) {
    CHECKSERT(conn, "Connection established with %s:%i", eth0.ip_addr().to_string().c_str(), 1337);
    verify();
  });

  INFO("UDP", "eth0 => listen:4444");
  eth0.udp().bind(4444).on_read([&](auto addr, auto, const char*, size_t) {
    CHECKSERT(addr == eth1.ip_addr(), "Received UDP data from eth1");
    verify();
  });
  std::string udp_data{"yolo"};
  eth1.udp().bind().sendto(eth0.ip_addr(), 3333, udp_data.data(), udp_data.size());


  // some breathing room
  Timers::oneshot(std::chrono::seconds(2), [](auto) {
    test_tcp_conntrack();
  });
}

#include <net/tcp/tcp_conntrack.hpp>
#include <statman>

void test_tcp_conntrack()
{
  static std::vector<char> storage;

  // same rules still apply
  static auto& eth0   = Super_stack::get(0);
  static auto& eth1   = Super_stack::get(1);
  static auto& host1  = Super_stack::get(2);
  static auto& host2  = Super_stack::get(3);

  // retrieve the shared conntrack instance
  eth0.conntrack()->tcp_in = net::tcp::tcp4_conntrack;

  // eth0 won't have seen the handshake so it will drop them packets
  static auto& stat = Statman::get().get_by_name("eth0.ip4.prerouting_dropped");
  static const auto drop_before = stat.get_uint32();

  const uint16_t port {6666};
  // hack to reduce close time so we don't have to wait forever
  // for the connection to close down, allowing us to end the
  // test when conn is closed
  host2.tcp().set_MSL(std::chrono::seconds(1));
  host2.tcp().listen(port, [](auto conn)
  {
    conn->on_read(1024, [conn](auto buf) {
      std::string str{(const char*)buf->data(), buf->size()};
      printf("Recv %s\n", str.c_str());
      if(str == "10")
        conn->close();
    });
    conn->on_close([]{
      const auto drop_after = stat.get_uint32();
      CHECKSERT((drop_after - drop_before) > 5,
        "At least 6 packets has been denied(dropped) before=%u after=%u", drop_before, drop_after);
      verify();
    });
  });

  host1.tcp().connect({host2.ip_addr(), port}, [](auto conn)
  {
    static int i = 0;

    while(i++ < 10)
    {
      auto n = i;
      Timers::oneshot(std::chrono::milliseconds(n*500), [n, conn](auto)
      {
        conn->write(std::to_string(n));

        if(n == 5)
        {
          printf("Store CT state\n");
          eth0.conntrack()->serialize_to(storage);
          printf("Assigning new CT, breaking established connections.\n");
          auto new_ct = std::make_shared<Conntrack>();
          new_ct->tcp_in = net::tcp::tcp4_conntrack;
          eth0.conntrack() = new_ct;
          eth1.conntrack() = new_ct;

          // restore it after 3 seconds, allowing connections to get through again
          Timers::oneshot(std::chrono::seconds(3), [](auto)
          {
            printf("Restoring CT state\n");
            eth0.conntrack()->deserialize_from(storage.data());
          });
        }
      });
    }
  });
}
