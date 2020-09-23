local event = 'custom';
local platforms = ['opensuse/leap:15', 'centos:7', 'centos:8', 'debian:9', 'debian:10', 'ubuntu:16.04', 'ubuntu:18.04', 'ubuntu:20.04'];

local builddir = 'verylongdirnameforverystrangecpackbehavior';

local cmakeflags = '-DCMAKE_BUILD_TYPE=RelWithDebInfo -DPLUGIN_COLUMNSTORE=YES -DPLUGIN_XPAND=NO -DPLUGIN_MROONGA=NO -DPLUGIN_ROCKSDB=NO -DPLUGIN_TOKUDB=NO -DPLUGIN_CONNECT=NO -DPLUGIN_SPIDER=NO -DPLUGIN_OQGRAPH=NO -DPLUGIN_SPHINX=NO -DBUILD_CONFIG=mysql_release -DWITH_WSREP=OFF';

local rpm_build_deps = 'install -y systemd-devel git make gcc gcc-c++ libaio-devel openssl-devel boost-devel bison snappy-devel flex libcurl-devel libxml2-devel ncurses-devel automake libtool policycoreutils-devel rpm-build lsof iproute pam-devel perl-DBI cracklib-devel expect readline-devel createrepo';

local deb_build_deps = 'apt update && apt install --yes --no-install-recommends dpkg-dev systemd libsystemd-dev git ca-certificates devscripts equivs build-essential libboost-all-dev libdistro-info-perl flex pkg-config automake libtool lsb-release bison chrpath cmake dh-apparmor dh-systemd gdb libaio-dev libcrack2-dev libjemalloc-dev libjudy-dev libkrb5-dev libncurses5-dev libpam0g-dev libpcre3-dev libreadline-gplv2-dev libsnappy-dev libssl-dev libsystemd-dev libxml2-dev unixodbc-dev uuid-dev zlib1g-dev libcurl4-openssl-dev dh-exec libpcre2-dev libzstd-dev psmisc socat expect net-tools rsync lsof libdbi-perl iproute2 gawk && mk-build-deps debian/control && dpkg -i mariadb-10*.deb || true && apt install -fy --no-install-recommends';

local platformMap(platform) =
  local platform_map = {
    'opensuse/leap:15': 'zypper ' + rpm_build_deps + ' cmake libboost_system-devel libboost_filesystem-devel libboost_thread-devel libboost_regex-devel libboost_date_time-devel libboost_chrono-devel libboost_atomic-devel && cmake ' + cmakeflags + ' -DRPM=sles15 && make -j$(nproc) package',
    'centos:7': 'yum install -y epel-release && yum install -y cmake3 && ln -s /usr/bin/cmake3 /usr/bin/cmake && yum ' + rpm_build_deps + ' && cmake ' + cmakeflags + ' -DRPM=centos7 && make -j$(nproc) package',
    'centos:8': "yum install -y libgcc && sed -i 's/enabled=0/enabled=1/' /etc/yum.repos.d/CentOS-PowerTools.repo && yum " + rpm_build_deps + ' cmake && cmake ' + cmakeflags + ' -DRPM=centos8 && make -j$(nproc) package',
    'debian:9': deb_build_deps + " && CMAKEFLAGS='" + cmakeflags + " -DDEB=stretch' debian/autobake-deb.sh",
    'debian:10': deb_build_deps + " && CMAKEFLAGS='" + cmakeflags + " -DDEB=buster' debian/autobake-deb.sh",
    'ubuntu:16.04': deb_build_deps + " && CMAKEFLAGS='" + cmakeflags + " -DDEB=xenial' debian/autobake-deb.sh",
    'ubuntu:18.04': deb_build_deps + " && CMAKEFLAGS='" + cmakeflags + " -DDEB=bionic' debian/autobake-deb.sh",
    'ubuntu:20.04': deb_build_deps + " && CMAKEFLAGS='" + cmakeflags + " -DDEB=focal' debian/autobake-deb.sh",
  };
  platform_map[platform];

local Pipeline(platform) = {
  local pkg_format = if (std.split(platform, ':')[0] == 'centos' || std.split(platform, ':')[0] == 'opensuse/leap') then 'rpm' else 'deb',
  local init = if (pkg_format == 'rpm') then '/usr/lib/systemd/systemd' else 'systemd',
  local img = if (std.split(platform, ':')[0] == 'centos') then platform else 'romcheck/' + std.strReplace(platform, '/', '-'),

  local pipeline = self,
  _volumes:: {
    mdb: {
      name: 'mdb',
      path: '/mdb',
    },
    docker: {
      name: 'docker',
      path: '/var/run/docker.sock',
    },
  },
  publish(step_prefix='pkg'):: {
    name: 'publish ' + step_prefix,
    image: 'plugins/s3-sync',
    when: {
      status: ['success', 'failure'],
    },
    settings: {
      bucket: 'cspkg',
      access_key: {
        from_secret: 'aws_access_key_id',
      },
      secret_key: {
        from_secret: 'aws_secret_access_key',
      },
      source: 'result',
      target: event + '/${DRONE_BUILD_NUMBER}/' + std.strReplace(std.strReplace(platform, ':', ''), '/', '-'),
      delete: 'true',
    },
  },
  regression:: {
    name: 'regression',
    image: 'docker:git',
    volumes: [pipeline._volumes.docker, pipeline._volumes.mdb],
    commands: [
      // clone regression test repo
      'git clone --recurse-submodules --branch ${REGRESSION_BRANCH:-develop} --depth 1 https://github.com/mariadb-corporation/mariadb-columnstore-regression-test',
      'docker run --volume /sys/fs/cgroup:/sys/fs/cgroup:ro --env DEBIAN_FRONTEND=noninteractive --env MCS_USE_S3_STORAGE=0 --name regression$${DRONE_BUILD_NUMBER} --privileged --detach ' + img + ' ' + init + ' --unit=basic.target',
      'docker cp result regression$${DRONE_BUILD_NUMBER}:/',
      'docker cp mariadb-columnstore-regression-test regression$${DRONE_BUILD_NUMBER}:/',
      if (std.split(platform, ':')[0] == 'centos') then 'docker exec -t regression$${DRONE_BUILD_NUMBER} bash -c "yum install -y epel-release diffutils tar lz4 wget git which rsyslog hostname && yum install -y /result/*.' + pkg_format + '"' else '',
      if (std.split(platform, ':')[0] == 'debian' || std.split(platform, ':')[0] == 'ubuntu') then 'docker exec -t regression$${DRONE_BUILD_NUMBER} bash -c "apt update && apt install -y tar liblz4-tool wget git rsyslog hostname && apt install -y -f /result/*.' + pkg_format + '"' else '',
      if (std.split(platform, '/')[0] == 'opensuse') then 'docker exec -t regression$${DRONE_BUILD_NUMBER} bash -c "zypper install -y gzip tar lz4 wget git which hostname rsyslog && zypper install -y --allow-unsigned-rpm /result/*.' + pkg_format + '"' else '',
      // copy  test data for regression test suite
      'docker exec -t regression$${DRONE_BUILD_NUMBER} bash -c "wget -qO- https://cspkg.s3.amazonaws.com/testData.tar.lz4 | lz4 -dc - | tar xf - -C mariadb-columnstore-regression-test/"',
      // set mariadb lower_case_table_names=1 config option
      'docker exec -t regression$${DRONE_BUILD_NUMBER} sed -i "/^.mariadb.$/a lower_case_table_names=1" /etc/' + (if pkg_format == 'deb' then 'mysql/mariadb.conf.d/50-' else 'my.cnf.d/') + 'server.cnf',
      // start mariadb and mariadb-columnstore services
      'docker exec -t regression$${DRONE_BUILD_NUMBER} systemctl start mariadb',
      'docker exec -t regression$${DRONE_BUILD_NUMBER} systemctl start mariadb-columnstore',
      'docker exec -t --workdir /mariadb-columnstore-regression-test/mysql/queries/nightly/alltest regression$${DRONE_BUILD_NUMBER} ./go.sh  --tests=${REGRESSION_TESTS:-test000.sh}',
    ],
  },
  regressionlog:: {
    name: 'regressionlog',
    image: 'docker',
    volumes: [pipeline._volumes.docker],
    commands: [
      'echo "---------- start columnstore regression short report ----------"',
      'docker exec -t --workdir /mariadb-columnstore-regression-test/mysql/queries/nightly/alltest regression$${DRONE_BUILD_NUMBER} cat go.log || echo "missing go.log"',
      'echo "---------- end columnstore regression short report ----------"',
      'echo',
      'docker cp regression$${DRONE_BUILD_NUMBER}:/mariadb-columnstore-regression-test/mysql/queries/nightly/alltest/testErrorLogs.tgz /drone/src/result/ || echo "missing testErrorLogs.tgz"',
      'docker stop regression$${DRONE_BUILD_NUMBER} && docker rm regression$${DRONE_BUILD_NUMBER} || echo "cleanup regression failure"',
    ],
    when: {
      status: ['success', 'failure'],
    },
  },

  kind: 'pipeline',
  type: 'docker',
  name: platform,
  clone: {
    disable: true,
  },

  steps: [
           {
             name: 'clone',
             image: 'alpine/git',
             volumes: [pipeline._volumes.mdb],
             commands: [
               'mkdir -p /mdb/' + builddir + ' && cd /mdb/' + builddir,
               'git config --global url."https://github.com/".insteadOf git@github.com:',
               'git clone --recurse-submodules --branch ${SERVER_BRANCH:-10.6} --depth 1 https://github.com/MariaDB/server .',
               'git config cmake.update-submodules no',
               'echo "server commit" && git rev-parse HEAD',
               'rm -rf storage/columnstore/columnstore',
               'git clone --recurse-submodules --branch ${ENGINE_BRANCH:-develop} --depth 1 https://github.com/mariadb-corporation/mariadb-columnstore-engine storage/columnstore/columnstore',
               'cd storage/columnstore/columnstore && echo "engine commit:" && git rev-parse HEAD',
               'git config cmake.update-submodules no',
             ],
           },
           {
             name: 'build',
             image: platform,
             volumes: [pipeline._volumes.mdb],
             environment: {
               DEBIAN_FRONTEND: 'noninteractive',
               TRAVIS: 'true',
             },
             commands: [
               'cd /mdb/' + builddir,
               "sed -i -e '/-DBUILD_CONFIG=mysql_release/d' debian/rules",
               "sed -i -e '/Package: libmariadbd19/,/^$/d' debian/control",
               "sed -i -e '/Package: libmariadbd-dev/,/^$/d' debian/control",
               "sed -i -e '/Package: mariadb-backup/,/^$/d' debian/control",
               "sed -i -e '/Package: mariadb-plugin-connect/,/^$/d' debian/control",
               "sed -i -e '/Package: mariadb-plugin-cracklib-password-check/,/^$/d' debian/control",
               "sed -i -e '/Package: mariadb-plugin-gssapi*/,/^$/d' debian/control",
               "sed -i -e '/Package: mariadb-plugin-xpand*/,/^$/d' debian/control",
               "sed -i -e '/wsrep/d' debian/mariadb-server-*.install",
               "sed -i -e 's/Depends: galera.*/Depends:/' debian/control",
               "sed -i -e 's/\"galera-enterprise-4\"//' cmake/cpack_rpm.cmake",
               "sed -i '/columnstore/Id' debian/autobake-deb.sh",
               "sed -i 's/.*flex.*/echo/' debian/autobake-deb.sh",
               "sed -i 's/-DPLUGIN_PERFSCHEMA=NO//' debian/autobake-deb.sh",
               platformMap(platform),
               (if pkg_format == 'rpm' then 'createrepo .' else 'dpkg-scanpackages ../ | gzip > ../Packages.gz '),
             ],
           },
           {
             name: 'list pkgs',
             image: 'docker:git',
             volumes: [pipeline._volumes.mdb],
             commands: [
               'cd /mdb/' + builddir,
               'mkdir /drone/src/result',
               'echo "engine: $DRONE_COMMIT" > buildinfo.txt',
               'echo "server: $$(git rev-parse HEAD)" >> buildinfo.txt',
               'echo "buildNo: $DRONE_BUILD_NUMBER" >> buildinfo.txt',
               'cp -r ' + (if pkg_format == 'deb' then '../Packages.gz ../' else 'repodata ') + '*.' + pkg_format + ' buildinfo.txt /drone/src/result/',
               'ls -l /drone/src/result',
               'echo "check columnstore package:"',
               'ls -l /drone/src/result | grep columnstore',
             ],
           },
         ] +
         [pipeline.publish()] +
         [pipeline.regression] +
         [pipeline.regressionlog] +
         [pipeline.publish('regression')],
  volumes: [pipeline._volumes.mdb { temp: {} }, pipeline._volumes.docker { host: { path: '/var/run/docker.sock' } }],
  trigger: {
    event: event,
  },
};

[
  Pipeline(p)
  for p in platforms
]
