# SPDX-License-Identifier: Apache-2.0
"""Cross-language wire compatibility and destination confinement."""

import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / "android/muserelay"


class PhoneRelay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.out = Path(cls.tmp.name)
        jdk = Path("/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin")
        cls.java = str(jdk / "java") if jdk.exists() else "java"
        javac = str(jdk / "javac") if jdk.exists() else "javac"
        subprocess.run(
            [
                javac,
                "-d",
                str(cls.out),
                str(JAVA / "src/com/musegadgets/relay/Protocol.java"),
                str(JAVA / "src/com/musegadgets/relay/PublicEndpoint.java"),
                str(JAVA / "tests/ProtocolTest.java"),
                str(JAVA / "src/com/musegadgets/relay/SocketPipe.java"),
                str(JAVA / "tests/SocketPipeTest.java"),
            ],
            check=True,
            capture_output=True,
        )
        source = cls.out / "parser.c"
        source.write_text("""#include <stdio.h>
#include "protocol.h"
int main(void) { char line[4096]; unsigned value;
 while(fgets(line,sizeof(line),stdin)) { uint8_t data[2048]; size_t n=0;
 for(size_t i=0;line[i] && line[i]!='\\n';i+=2) { if(sscanf(line+i,"%2x",&value)!=1)return 2; data[n++]=value; }
 printf("%d\\n",relay_valid(data,n)); } return 0; }
""")
        cls.parser = cls.out / "parser"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I",
                str(ROOT / "esp32/main/phone_relay"),
                str(source),
                "-o",
                str(cls.parser),
            ],
            check=True,
            capture_output=True,
        )

    def test_java_roundtrips_and_public_addresses(self):
        subprocess.run(
            [self.java, "-cp", str(self.out), "com.musegadgets.relay.ProtocolTest"],
            check=True,
            capture_output=True,
        )

    def test_c_java_accept_identical_corpus(self):
        rng = random.Random(17)
        corpus = [
            bytes(rng.randrange(256) for _ in range(rng.randrange(200)))
            for _ in range(3000)
        ]
        for t in range(10):
            for stream in (0, 1, 65535):
                for epoch in (0, 1, 0xFFFFFFFF):
                    for size in (0, 1, 12, 180, 181):
                        header = (
                            bytes([1, t])
                            + stream.to_bytes(2, "little")
                            + epoch.to_bytes(4, "little")
                        )
                        corpus.append(header + bytes([1]) * size)
        data = "".join(p.hex() + "\n" for p in corpus)
        c = subprocess.run(
            [str(self.parser)], input=data, text=True, capture_output=True, check=True
        ).stdout
        java = subprocess.run(
            [
                self.java,
                "-cp",
                str(self.out),
                "com.musegadgets.relay.ProtocolTest",
                "corpus",
            ],
            input=data,
            text=True,
            capture_output=True,
            check=True,
        ).stdout
        self.assertEqual(c, java)
        self.assertIn("1", c)

    def test_socket_flow_control(self):
        subprocess.run(
            [self.java, "-cp", str(self.out), "com.musegadgets.relay.SocketPipeTest"],
            check=True,
            capture_output=True,
            timeout=15,
        )

    def test_firmware_socket_bridge(self):
        binary = self.out / "firmware"
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-D_DARWIN_C_SOURCE",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-pthread",
                "-I",
                str(ROOT / "esp32/tests/phone_relay_fakes"),
                str(ROOT / "esp32/tests/phone_relay_harness.c"),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run([str(binary)], check=True, capture_output=True, timeout=15)

    def test_socket_routes(self):
        inc = self.out / "lwip"
        inc.mkdir(exist_ok=True)
        (inc / "sockets.h").write_text(
            "#include <sys/socket.h>\n#include <netinet/in.h>\n#include <arpa/inet.h>\n"
        )
        (inc / "netdb.h").write_text("#include <netdb.h>\n")
        (self.out / "sdkconfig.h").write_text("#define CONFIG_MUSE_PHONE_RELAY 1\n")
        source = self.out / "routes.c"
        source.write_text(r"""#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
static bool ready;
static int connects;
bool muse_phone_relay_ready(void){return ready;}
int __real_lwip_getaddrinfo(const char *h,const char *s,const struct addrinfo *a,struct addrinfo **o){return getaddrinfo(h,s,a,o);}
int __real_lwip_connect(int fd,const struct sockaddr *a,socklen_t n){
 assert(fd==7 && n==sizeof(struct sockaddr_in));const struct sockaddr_in *v=(const void*)a;
 assert(ntohl(v->sin_addr.s_addr)==INADDR_LOOPBACK);assert(ntohs(v->sin_port)==18443 || ntohs(v->sin_port)==18444); connects++;return 0;}
int __wrap_lwip_getaddrinfo(const char*,const char*,const struct addrinfo*,struct addrinfo**);
int __wrap_lwip_connect(int,const struct sockaddr*,socklen_t);
int main(void){struct addrinfo *out=NULL;
 assert(__wrap_lwip_getaddrinfo("api.muse.ai",NULL,NULL,&out)!=0 && !out);ready=true;
 const char *hosts[]={"api.muse.ai","hatch.metaaivm.com"};
 for(unsigned i=0;i<2;i++) {assert(!__wrap_lwip_getaddrinfo(hosts[i],NULL,NULL,&out));
 struct sockaddr_in a=*(struct sockaddr_in*)out->ai_addr;freeaddrinfo(out);
 assert(ntohl(a.sin_addr.s_addr)==0xc6120001u+i);a.sin_port=htons(443);
 assert(!__wrap_lwip_connect(7,(void*)&a,sizeof(a)));a.sin_port=htons(80);
 assert(__wrap_lwip_connect(7,(void*)&a,sizeof(a))==-1);}
 for(unsigned i=0;i<3;i++){const char *bad[]={"localhost","example.com","127.0.0.1"};out=NULL;assert(__wrap_lwip_getaddrinfo(bad[i],NULL,NULL,&out)!=0 && !out);}
 struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(443),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(__wrap_lwip_connect(7,(void*)&a,sizeof(a))==-1);assert(connects==2);
 ready=false;assert(__wrap_lwip_connect(7,(void*)&a,sizeof(a))==-1);return 0;}
""")
        binary = self.out / "routes"
        subprocess.run(
            [
                "cc",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I",
                str(self.out),
                str(source),
                str(ROOT / "esp32/main/phone_relay/socket_routes.c"),
                "-o",
                str(binary),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run([str(binary)], check=True, capture_output=True)
