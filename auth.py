import teslapy
with teslapy.Tesla(email=input('Enter e-mail: '), cache_file='/var/tmp/tesla_cron.json') as tesla:
    if not tesla.authorized:
        tesla.refresh_token(refresh_token=input('Enter SSO refresh token: '))
        vehicles = tesla.vehicle_list()
        print(vehicles[0])

