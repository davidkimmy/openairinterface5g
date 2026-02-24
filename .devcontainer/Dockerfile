FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
#ENV BUILD_UHD_FROM_SOURCE=True
#ENV UHD_VERSION=4.7.0.0

RUN apt-get update && \
    apt-get install -y sudo cmake build-essential libsimde-dev \
    libsctp-dev libconfig-dev psmisc xxd vim \
    net-tools iputils-ping clangd-20 && \
    apt-get clean &&  rm -rf /var/lib/apt/lists/*

RUN touch /var/mail/ubuntu && chown ubuntu /var/mail/ubuntu && userdel -r ubuntu

RUN update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-20 100

ARG USERNAME

# Create a non-root user with a home directory and default shell
RUN useradd -ms /bin/bash ${USERNAME}

# Add the user to the sudo group with sudo privileges by default
RUN usermod -aG sudo ${USERNAME}

# This means the user can run any command as root without a password.
RUN echo "${USERNAME} ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

# Switch to the new non-root user
USER $USERNAME

# Set the working directory
WORKDIR /home/$USERNAME

# Command to run when the container starts
CMD ["/bin/bash"]

RUN ln -s /workspaces/openairinterface5g /home/$USERNAME/openairinterface5g