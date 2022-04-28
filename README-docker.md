# To get a Docker build

Note that the docker support is not complete yet!

* Check out the code
* Build the docker file, using `docker build . -t tesla-cron`
* Obtain a Tesla refresh token for your account. This can be generated at eg TeslaFi or the Tesla Access Token Generator chrome extension.
* Run the docker file, using something similar to `docker run -e TESLA_ACCOUNT_EMAIL=<your account email> -e TESLA_SSO_REFRESH_TOKEN=<your refresh token> tesla-cron`

Thats it, really.
