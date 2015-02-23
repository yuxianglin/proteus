FROM ubuntu:14.04

MAINTAINER Chris Kees <cekees@gmail.com>

ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update && apt-get install -y -q language-pack-en
ENV LANGUAGE en_US.UTF-8
ENV LANG en_US.UTF-8
ENV LC_ALL en_US.UTF-8

RUN locale-gen en_US.UTF-8
RUN dpkg-reconfigure locales

RUN apt-get install -y -q \
    build-essential \
    make \
    gcc \
    gfortran \
    git \
    python \
    libcurl4-openssl-dev \
    libblas-dev \
    liblapack-dev \
    cmake

RUN useradd -m -s /bin/bash jovyan
USER jovyan
WORKDIR /home/jovyan
RUN git clone https://github.com/erdc-cm/proteus
WORKDIR proteus
RUN make stack
WORKDIR  /home/jovyan/proteus/stack
RUN git checkout cekees/test_tmpnb
WORKDIR /home/jovyan/proteus
RUN make
ENV PATH /home/jovyan/proteus/linux2/bin:$PATH
ENV LD_LIBRARY_PATH /home/jovyan/proteus/linux2/lib:$LD_LIBRARY_PATH
RUN ipython profile create

# Workaround for issue with ADD permissions
USER root
ADD profile_default /home/jovyan/.ipython/profile_default

# Fake Google Analytics directory (for now)
ADD ga/ /srv/ga/
RUN chmod a+rX /srv/ga
RUN chown jovyan:jovyan /home/jovyan -R

EXPOSE 8888

USER jovyan
ENV HOME /home/jovyan
ENV SHELL /bin/bash
ENV USER jovyan

WORKDIR /home/jovyan/
RUN rm -rf .ipython
RUN ipython profile create default
CMD ["ipython","notebook"]
