# To get a Docker build

* Check out the code
* Clone the submodules using `git submodule update --init --recursive`
* Build the docker file, using `docker build . -t tesla-cron`
* Obtain a Tesla refresh token for your account. This can be generated at eg TeslaFi or the Tesla Access Token Generator chrome extension.
* Run the docker file, using something similar to `docker run -e TESLA_ACCOUNT_EMAIL=<your account email> -e TESLA_SSO_REFRESH_TOKEN=<your refresh token>`

Thats it, really.
